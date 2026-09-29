#include "trackstar/botservice.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTcpSocket>
#include <QThread>

#include <signal.h>

#include "util/logger.h"

namespace {

const mixxx::Logger kLogger("TrackStarBot");
const QString kGroup = QStringLiteral("[TrackStar]");
const QString kBundleExe = QStringLiteral(
        "/Applications/TrackStar DJ Bot.app/Contents/MacOS/TrackStar DJ Bot");

qint64 s_lastStartMs = 0;
qint64 s_pid = 0; // the helper we started (via sh -c exec, so this is the helper itself)

QString resolveCommand(UserSettingsPointer pConfig) {
    const QString env = QProcessEnvironment::systemEnvironment().value(
            QStringLiteral("TRACKSTAR_BOT_CMD"));
    if (!env.trimmed().isEmpty()) {
        return env;
    }
    if (pConfig) {
        const QString cfg = pConfig->getValueString(ConfigKey(kGroup, QStringLiteral("bot_command")));
        if (!cfg.trimmed().isEmpty()) {
            return cfg;
        }
    }
    // The helper ships inside the Mixer bundle (Contents/Helpers), a standalone Bot app is the fallback.
    const QString embedded = QCoreApplication::applicationDirPath() +
            QStringLiteral("/../Helpers/TrackStar DJ Bot.app/Contents/MacOS/TrackStar DJ Bot");
    if (QFileInfo::exists(embedded)) {
        return QStringLiteral("'%1' --serve").arg(QFileInfo(embedded).canonicalFilePath());
    }
    if (QFileInfo::exists(kBundleExe)) {
        return QStringLiteral("'%1' --serve").arg(kBundleExe);
    }
    return QString();
}

} // namespace

namespace trackstar {

bool localPortOpen(int port, int timeoutMs) {
    QTcpSocket socket;
    socket.connectToHost(QStringLiteral("127.0.0.1"), static_cast<quint16>(port));
    const bool ok = socket.waitForConnected(timeoutMs);
    socket.abort();
    return ok;
}

QString botServiceUrl() {
    return QStringLiteral("http://127.0.0.1:%1/?embed=mixer").arg(kBotServicePort);
}

bool ensureBotService(UserSettingsPointer pConfig, int waitForLinkMs) {
    if (pConfig &&
            pConfig->getValueString(ConfigKey(kGroup, QStringLiteral("bot_autostart"))) ==
                    QStringLiteral("0")) {
        return false;
    }
    if (localPortOpen(kBotServicePort)) {
        return true;
    }
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (s_lastStartMs && now - s_lastStartMs < 20000) {
        return true; // still starting up
    }
#ifndef _WIN32
    if (s_pid > 0 && ::kill(static_cast<pid_t>(s_pid), 0) == 0) {
        // Ours is still alive but does not answer (yet): never start a second one (an old Bot bundle
        // without --serve would open a window each time).
        return true;
    }
#endif
    const QString cmd = resolveCommand(pConfig);
    if (cmd.isEmpty()) {
        kLogger.info() << "no Bot helper found (TRACKSTAR_BOT_CMD, [TrackStar],bot_command or"
                       << kBundleExe << ")";
        return false;
    }
    s_lastStartMs = now;
    const QString log = QDir::homePath() + QStringLiteral("/Library/Logs/TrackStar DJ Bot service.log");
    const QString shell = QStringLiteral("exec %1 >>'%2' 2>&1 </dev/null").arg(cmd, log);
    const bool started = QProcess::startDetached(QStringLiteral("/bin/sh"),
            {QStringLiteral("-c"), shell},
            QString(),
            &s_pid);
    kLogger.info() << "starting Bot helper:" << cmd << (started ? "ok" : "FAILED");
    if (!started) {
        return false;
    }
    if (waitForLinkMs > 0 && !localPortOpen(kBotLinkPort)) {
        // The RobotDJ MIDI port must exist before the controllers are scanned (no MIDI hotplug).
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < waitForLinkMs && !localPortOpen(kBotLinkPort, 100)) {
            QThread::msleep(100);
        }
        kLogger.info() << "RobotDJ link" << (localPortOpen(kBotLinkPort) ? "up" : "not up")
                       << "after" << timer.elapsed() << "ms";
    }
    return true;
}

} // namespace trackstar
