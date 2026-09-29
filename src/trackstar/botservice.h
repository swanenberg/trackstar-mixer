#pragma once

// TrackStar Bot service: the Bot's engine and UI run as a windowless helper process (TrackStar DJ Bot
// with --serve) that serves its page on 127.0.0.1:7913. The Mixer shows that page in place of the
// library (WTrackStarBot) and starts the helper itself, so there is no separate Bot app to open.
//
// Which command starts the helper:
//   1. environment variable TRACKSTAR_BOT_CMD (run through /bin/sh -c; the dev start script sets it
//      to the repo checkout),
//   2. config [TrackStar],bot_command,
//   3. the helper inside the Mixer bundle: Contents/Helpers/TrackStar DJ Bot.app (… --serve),
//   4. /Applications/TrackStar DJ Bot.app/Contents/MacOS/TrackStar DJ Bot --serve.
// [TrackStar],bot_autostart = 0 switches the autostart off.

#include <QString>

#include "preferences/usersettings.h"

namespace trackstar {

constexpr int kBotServicePort = 7913;
constexpr int kBotLinkPort = 7911; // the RobotDJ MIDI daemon the helper starts first

/// True when something accepts connections on 127.0.0.1:port (blocking, at most timeoutMs).
bool localPortOpen(int port, int timeoutMs = 150);

/// Start the helper when it is not running (and not started less than 20 s ago).
/// With waitForLinkMs > 0 it waits for the RobotDJ MIDI port, so that the controller scan at startup
/// sees it. Returns false when no command was found or autostart is off.
bool ensureBotService(UserSettingsPointer pConfig, int waitForLinkMs = 0);

/// The page the Mixer shows.
QString botServiceUrl();

} // namespace trackstar
