#pragma once

// TrackStar bridge: a small local TCP server (JSON per line) that lets the TrackStar DJ suite
// (the Bot, robotdj.engine) read and set controls, load a track by library id into a deck and
// receive events — without the virtual-MIDI/SysEx detour through a controller script.
//
//   {"id":1,"cmd":"ping"}                                   -> {"id":1,"pong":true,"version":"…","bridge":true}
//   {"id":2,"cmd":"get","group":"[Channel1]","key":"play"}  -> {"id":2,"value":1}
//   {"id":3,"cmd":"set","group":"[Channel1]","key":"volume","value":0.8}
//   {"id":4,"cmd":"load_id","group":"[Channel2]","track_id":2101,"play":false}
//   {"id":5,"cmd":"subscribe","keys":[["[Channel1]","play"]]}   events: {"ev":"co","group":..,"key":..,"value":..}
//   {"id":6,"cmd":"stream","interval":250}                 events: {"ev":"pos","d":{"[Channel1]":{...},...}}
// Events without a request: {"ev":"hello"}, {"ev":"track_loaded","group":..,"track":{...}}, {"ev":"play",...},
// {"ev":"handoff_button","value":..} ([TrackStar],handoff — created here, skin independent).
//
// Lives on the main thread (created by CoreServices), so PlayerManager and TrackCollectionManager
// may be called directly. Listens on 127.0.0.1 only.

#include <QByteArray>
#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QSet>
#include <QString>
#include <QTcpServer>
#include <QTimer>
#include <memory>
#include <vector>

#include "control/controlproxy.h"
#include "control/controlpushbutton.h"
#include "preferences/usersettings.h"

class PlayerManager;
class TrackCollectionManager;
class QTcpSocket;

class TrackStarBridge : public QObject {
    Q_OBJECT
  public:
    static constexpr int kDefaultPort = 7912;

    TrackStarBridge(PlayerManager* pPlayerManager,
            TrackCollectionManager* pTrackCollectionManager,
            UserSettingsPointer pConfig,
            QObject* pParent = nullptr);
    ~TrackStarBridge() override;

    bool isListening() const;

  private slots:
    void slotNewConnection();
    void slotReadyRead();
    void slotDisconnected();
    void slotStreamTick();

  private:
    struct Watch {
        QString group;
        QString key;
        ControlProxy* proxy; // parented to the bridge
    };

    QJsonObject handle(const QJsonObject& msg, QTcpSocket* pClient);
    void send(QTcpSocket* pClient, const QJsonObject& obj);
    void broadcast(const QJsonObject& obj);
    ControlProxy* proxy(const QString& group, const QString& key);
    double value(const QString& group, const QString& key, double fallback = 0.0);
    QJsonObject trackInfo(const QString& group);
    bool watch(const QString& group, const QString& key);
    QJsonObject loadTrackById(const QJsonObject& msg);

    PlayerManager* m_pPlayerManager;
    TrackCollectionManager* m_pTrackCollectionManager;
    UserSettingsPointer m_pConfig;

    QTcpServer m_server;
    QList<QTcpSocket*> m_clients;
    QHash<QTcpSocket*, QByteArray> m_buffers;
    QHash<QString, ControlProxy*> m_proxies; // "group,key" -> proxy (read/write), parented to this
    QHash<QString, Watch*> m_watches;        // "group,key" -> watched control (deleted in ~TrackStarBridge)
    QHash<QString, qint64> m_lastLoadMs;                     // group -> last load (double-tap guard)

    std::unique_ptr<ControlPushButton> m_pHandoff;
    std::unique_ptr<ControlPushButton> m_pHumanDetect;
    std::unique_ptr<ControlProxy> m_pHandoffWatch;
    std::vector<std::unique_ptr<ControlPushButton>> m_suiteButtons;
    std::vector<std::unique_ptr<ControlProxy>> m_suiteWatches;

    QTimer m_streamTimer;
    QStringList m_streamGroups;
};
