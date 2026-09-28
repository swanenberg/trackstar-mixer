#include "trackstar/bridge.h"

#include <QApplication>
#include <QDateTime>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTcpSocket>
#include <QVariant>
#include <QtDebug>

#include "control/controlobject.h"
#include "engine/engine.h"
#include "library/trackcollectionmanager.h"
#include "mixer/basetrackplayer.h"
#include "mixer/playermanager.h"
#include "track/track.h"
#include "util/versionstore.h"
#include "widget/wlabel.h"

namespace {
const QString kGroup = QStringLiteral("[TrackStar]");
const QString kBridgeVersion = QStringLiteral("0.2.0");
constexpr qint64 kDoubleTapGuardMs = 600; // PlayerManager clones the deck on a 2nd load within 0.5 s

QString slotKey(const QString& group, const QString& key) {
    return group + QChar(',') + key;
}
} // namespace

TrackStarBridge::TrackStarBridge(PlayerManager* pPlayerManager,
        TrackCollectionManager* pTrackCollectionManager,
        UserSettingsPointer pConfig,
        QObject* pParent)
        : QObject(pParent),
          m_pPlayerManager(pPlayerManager),
          m_pTrackCollectionManager(pTrackCollectionManager),
          m_pConfig(pConfig) {
    // Suite controls, independent of the skin: the BOT button in the skin binds to these.
    m_pHandoff = std::make_unique<ControlPushButton>(ConfigKey(kGroup, QStringLiteral("handoff")), true, 0.0);
    m_pHandoff->setButtonMode(mixxx::control::ButtonMode::Toggle);
    m_pHumanDetect = std::make_unique<ControlPushButton>(
            ConfigKey(kGroup, QStringLiteral("human_detect")), true, 1.0);
    m_pHumanDetect->setButtonMode(mixxx::control::ButtonMode::Toggle);
    m_pHandoffWatch = std::make_unique<ControlProxy>(ConfigKey(kGroup, QStringLiteral("handoff")), this);
    m_pHandoffWatch->connectValueChanged(this, [this](double v) {
        broadcast(QJsonObject{{"ev", "handoff_button"}, {"value", v}});
    });

    // Deck events: track loaded / play, like the RobotDJ controller script sends them.
    const int decks = m_pPlayerManager ? m_pPlayerManager->numberOfDecks() : 0;
    for (int i = 0; i < decks; ++i) {
        const QString group = PlayerManager::groupForDeck(i);
        watch(group, QStringLiteral("track_loaded"));
        watch(group, QStringLiteral("play"));
    }

    connect(&m_server, &QTcpServer::newConnection, this, &TrackStarBridge::slotNewConnection);
    connect(&m_streamTimer, &QTimer::timeout, this, &TrackStarBridge::slotStreamTick);

    const int port = m_pConfig
            ? m_pConfig->getValue(ConfigKey(kGroup, QStringLiteral("bridge_port")), kDefaultPort)
            : kDefaultPort;
    if (port > 0 && m_server.listen(QHostAddress::LocalHost, static_cast<quint16>(port))) {
        qInfo() << "TrackStar bridge listening on 127.0.0.1:" << port;
    } else if (port > 0) {
        qWarning() << "TrackStar bridge could not listen on port" << port << ":" << m_server.errorString();
    }
}

TrackStarBridge::~TrackStarBridge() {
    m_streamTimer.stop();
    m_server.close();
    for (QTcpSocket* pClient : m_clients) {
        pClient->disconnect(this);
        pClient->close();
        pClient->deleteLater();
    }
    m_clients.clear();
    qDeleteAll(m_watches); // the proxies inside are children of this object
    m_watches.clear();
    m_proxies.clear();
    m_pHandoffWatch.reset();
}

bool TrackStarBridge::isListening() const {
    return m_server.isListening();
}

// ---------------------------------------------------------------- connections

void TrackStarBridge::slotNewConnection() {
    while (QTcpSocket* pClient = m_server.nextPendingConnection()) {
        m_clients.append(pClient);
        m_buffers[pClient] = QByteArray();
        connect(pClient, &QTcpSocket::readyRead, this, &TrackStarBridge::slotReadyRead);
        connect(pClient, &QTcpSocket::disconnected, this, &TrackStarBridge::slotDisconnected);
        send(pClient,
                QJsonObject{{"ev", "hello"},
                        {"version", VersionStore::version()},
                        {"bridge", kBridgeVersion}});
    }
}

void TrackStarBridge::slotDisconnected() {
    auto* pClient = qobject_cast<QTcpSocket*>(sender());
    if (!pClient) {
        return;
    }
    m_clients.removeAll(pClient);
    m_buffers.remove(pClient);
    pClient->deleteLater();
    if (m_clients.isEmpty()) {
        m_streamTimer.stop();
    }
}

void TrackStarBridge::slotReadyRead() {
    auto* pClient = qobject_cast<QTcpSocket*>(sender());
    if (!pClient) {
        return;
    }
    QByteArray& buf = m_buffers[pClient];
    buf.append(pClient->readAll());
    int nl;
    while ((nl = buf.indexOf('\n')) >= 0) {
        const QByteArray line = buf.left(nl).trimmed();
        buf.remove(0, nl + 1);
        if (line.isEmpty()) {
            continue;
        }
        QJsonParseError err{};
        const QJsonDocument doc = QJsonDocument::fromJson(line, &err);
        if (err.error != QJsonParseError::NoError || !doc.isObject()) {
            send(pClient, QJsonObject{{"err", QStringLiteral("bad json: %1").arg(err.errorString())}});
            continue;
        }
        const QJsonObject msg = doc.object();
        QJsonObject reply = handle(msg, pClient);
        if (msg.contains("id")) {
            reply.insert("id", msg.value("id"));
        }
        send(pClient, reply);
    }
    if (buf.size() > 1 << 20) { // a megabyte without a newline: not our protocol
        buf.clear();
    }
}

void TrackStarBridge::send(QTcpSocket* pClient, const QJsonObject& obj) {
    if (!pClient || pClient->state() != QAbstractSocket::ConnectedState) {
        return;
    }
    pClient->write(QJsonDocument(obj).toJson(QJsonDocument::Compact) + '\n');
}

void TrackStarBridge::broadcast(const QJsonObject& obj) {
    for (QTcpSocket* pClient : m_clients) {
        send(pClient, obj);
    }
}

// ---------------------------------------------------------------- controls

ControlProxy* TrackStarBridge::proxy(const QString& group, const QString& key) {
    const QString id = slotKey(group, key);
    auto it = m_proxies.find(id);
    if (it != m_proxies.end()) {
        return it.value();
    }
    auto* pProxy = new ControlProxy(ConfigKey(group, key), this, ControlFlag::AllowMissingOrInvalid);
    if (!pProxy->valid()) {
        delete pProxy;
        return nullptr; // not cached: the control may appear later (e.g. after a skin change)
    }
    m_proxies.insert(id, pProxy);
    return pProxy;
}

double TrackStarBridge::value(const QString& group, const QString& key, double fallback) {
    ControlProxy* p = proxy(group, key);
    return p ? p->get() : fallback;
}

bool TrackStarBridge::watch(const QString& group, const QString& key) {
    const QString id = slotKey(group, key);
    if (m_watches.contains(id)) {
        return true;
    }
    auto* pProxy = new ControlProxy(ConfigKey(group, key), this, ControlFlag::AllowMissingOrInvalid);
    if (!pProxy->valid()) {
        delete pProxy;
        return false;
    }
    auto* pRaw = new Watch{group, key, pProxy};
    pProxy->connectValueChanged(this, [this, pRaw](double v) {
        QJsonObject ev{{"ev", "co"}, {"group", pRaw->group}, {"key", pRaw->key}, {"value", v}};
        if (pRaw->key == QLatin1String("track_loaded")) {
            // duration/file_bpm/track_samples are updated right after track_loaded flips: report a
            // moment later so the event carries the complete track info.
            const QString group = pRaw->group;
            QTimer::singleShot(60, this, [this, group, v]() {
                broadcast(QJsonObject{{"ev", "track_loaded"},
                        {"group", group},
                        {"value", v},
                        {"track", trackInfo(group)}});
            });
            return;
        }
        if (pRaw->key == QLatin1String("play")) {
            ev.insert("ev", "play");
        }
        broadcast(ev);
    });
    m_watches.insert(id, pRaw);
    return true;
}

QJsonObject TrackStarBridge::trackInfo(const QString& group) {
    QJsonObject t;
    t.insert("loaded", value(group, "track_loaded"));
    t.insert("duration", value(group, "duration"));
    t.insert("bpm", value(group, "file_bpm"));
    t.insert("samples", value(group, "track_samples"));
    t.insert("samplerate", value(group, "track_samplerate"));
    t.insert("key", value(group, "file_key"));
    t.insert("stems", value(group, "stem_count"));
    t.insert("play", value(group, "play"));
    const double pos = value(group, "playposition");
    t.insert("pos", pos);
    t.insert("sample", pos * value(group, "track_samples"));
    t.insert("rate", value(group, "rate"));
    t.insert("rate_ratio", value(group, "rate_ratio", 1.0));
    if (m_pPlayerManager) {
        if (BaseTrackPlayer* pPlayer = m_pPlayerManager->getPlayer(group)) {
            if (TrackPointer pTrack = pPlayer->getLoadedTrack()) {
                t.insert("track_id", pTrack->getId().toVariant().toInt());
                t.insert("title", pTrack->getTitle());
                t.insert("artist", pTrack->getArtist());
            }
        }
    }
    return t;
}

QJsonObject TrackStarBridge::loadTrackById(const QJsonObject& msg) {
    if (!m_pPlayerManager || !m_pTrackCollectionManager) {
        return {{"err", "no player manager"}};
    }
    const QString group = msg.value("group").toString();
    const int id = msg.value("track_id").toInt(-1);
    if (group.isEmpty() || id < 0) {
        return {{"err", "load_id needs group and track_id"}};
    }
    if (!m_pPlayerManager->getPlayer(group)) {
        return {{"err", QStringLiteral("unknown group %1").arg(group)}};
    }
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const qint64 last = m_lastLoadMs.value(group, 0);
    if (now - last < kDoubleTapGuardMs) {
        // a second load within 0.5 s would clone the deck instead (CloneDeckOnLoadDoubleTap)
        return {{"ok", false}, {"err", "too soon after the previous load"}, {"retry_ms", kDoubleTapGuardMs - (now - last)}};
    }
    TrackPointer pTrack = m_pTrackCollectionManager->getTrackById(TrackId(QVariant(id)));
    if (!pTrack) {
        return {{"ok", false}, {"err", QStringLiteral("unknown track id %1").arg(id)}};
    }
    m_lastLoadMs[group] = now;
    const bool play = msg.value("play").toBool(false);
#ifdef __STEM__
    m_pPlayerManager->slotLoadTrackToPlayer(pTrack, group, mixxx::StemChannelSelection(), play);
#else
    m_pPlayerManager->slotLoadTrackToPlayer(pTrack, group, play);
#endif
    return {{"ok", true}, {"track_id", id}, {"title", pTrack->getTitle()}, {"artist", pTrack->getArtist()}};
}

// ---------------------------------------------------------------- commands

QJsonObject TrackStarBridge::handle(const QJsonObject& msg, QTcpSocket* pClient) {
    const QString cmd = msg.value("cmd").toString();
    const QString group = msg.value("group").toString();
    const QString key = msg.value("key").toString();

    if (cmd == QLatin1String("ping")) {
        return {{"pong", true}, {"version", VersionStore::version()}, {"bridge", kBridgeVersion}};
    }
    if (cmd == QLatin1String("get")) {
        ControlProxy* p = proxy(group, key);
        if (!p) {
            return {{"err", QStringLiteral("unknown control %1").arg(slotKey(group, key))}};
        }
        return {{"value", p->get()}};
    }
    if (cmd == QLatin1String("getmany")) {
        QJsonObject values;
        for (const QJsonValue& item : msg.value("items").toArray()) {
            const QJsonArray pair = item.toArray();
            if (pair.size() != 2) {
                continue;
            }
            const QString g = pair.at(0).toString(), k = pair.at(1).toString();
            ControlProxy* p = proxy(g, k);
            values.insert(slotKey(g, k), p ? QJsonValue(p->get()) : QJsonValue());
        }
        return {{"values", values}};
    }
    if (cmd == QLatin1String("set") || cmd == QLatin1String("setp")) {
        ControlProxy* p = proxy(group, key);
        if (!p) {
            return {{"err", QStringLiteral("unknown control %1").arg(slotKey(group, key))}};
        }
        const double v = msg.value("value").toDouble();
        if (cmd == QLatin1String("set")) {
            p->set(v);
        } else {
            p->setParameter(v);
        }
        return {{"ok", true}};
    }
    if (cmd == QLatin1String("track")) {
        return {{"track", trackInfo(group)}};
    }
    if (cmd == QLatin1String("seek")) {
        ControlProxy* p = proxy(group, QStringLiteral("playposition"));
        const double samples = value(group, "track_samples");
        if (!p || samples <= 0) {
            return {{"err", QStringLiteral("no track in %1").arg(group)}};
        }
        p->set(msg.value("sample").toDouble() / samples);
        return {{"ok", true}, {"sample", p->get() * samples}};
    }
    if (cmd == QLatin1String("load_id")) {
        return loadTrackById(msg);
    }
    if (cmd == QLatin1String("subscribe")) {
        int n = 0;
        for (const QJsonValue& item : msg.value("keys").toArray()) {
            const QJsonArray pair = item.toArray();
            if (pair.size() == 2 && watch(pair.at(0).toString(), pair.at(1).toString())) {
                ++n;
            }
        }
        return {{"ok", true}, {"watching", n}};
    }
    if (cmd == QLatin1String("stream")) {
        const int interval = msg.value("interval").toInt(250);
        m_streamGroups.clear();
        for (const QJsonValue& g : msg.value("groups").toArray()) {
            m_streamGroups.append(g.toString());
        }
        if (m_streamGroups.isEmpty() && m_pPlayerManager) {
            for (int i = 0; i < m_pPlayerManager->numberOfDecks(); ++i) {
                m_streamGroups.append(PlayerManager::groupForDeck(i));
            }
        }
        if (interval <= 0) {
            m_streamTimer.stop();
        } else {
            m_streamTimer.start(qMax(50, interval));
        }
        return {{"ok", true}};
    }
    if (cmd == QLatin1String("decks")) {
        QJsonArray groups;
        if (m_pPlayerManager) {
            for (int i = 0; i < m_pPlayerManager->numberOfDecks(); ++i) {
                groups.append(PlayerManager::groupForDeck(i));
            }
        }
        return {{"decks", groups}};
    }
    if (cmd == QLatin1String("status")) {
        return {{"connected", true}, {"clients", m_clients.size()}, {"bridge", kBridgeVersion}};
    }
    if (cmd == QLatin1String("bot_status")) {
        // Text for the "TrackStarBotStatus" label in the skin topbar (the Bot's
        // state / next transition), pushed by robotdj/engine.py. Empty = hide.
        const QString text = msg.value("text").toString();
        int found = 0;
        const auto topLevel = QApplication::topLevelWidgets();
        for (QWidget* pTop : topLevel) {
            const auto labels = pTop->findChildren<WLabel*>(QStringLiteral("TrackStarBotStatus"));
            for (WLabel* pLabel : labels) {
                pLabel->setText(text);
                pLabel->setVisible(!text.isEmpty());
                found++;
            }
        }
        return {{"ok", true}, {"labels", found}};
    }
    Q_UNUSED(pClient);
    return {{"err", QStringLiteral("unknown cmd %1").arg(cmd)}};
}

void TrackStarBridge::slotStreamTick() {
    if (m_clients.isEmpty()) {
        m_streamTimer.stop();
        return;
    }
    QJsonObject d;
    for (const QString& g : m_streamGroups) {
        const double pos = value(g, "playposition");
        d.insert(g,
                QJsonObject{{"play", value(g, "play")},
                        {"sample", pos * value(g, "track_samples")},
                        {"bd", value(g, "beat_distance")},
                        {"bpm", value(g, "bpm")},
                        {"vol", value(g, "volume")}});
    }
    broadcast(QJsonObject{{"ev", "pos"}, {"d", d}});
}

#include "moc_bridge.cpp"
