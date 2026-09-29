#include "trackstar/wtrackstarbot.h"

#include <QLabel>
#include <QStackedLayout>
#include <QTcpSocket>
#include <QWindow>

#include "moc_wtrackstarbot.cpp"
#include "trackstar/botservice.h"
#include "trackstar/webview.h"

namespace {
constexpr int kProbeIntervalMs = 1500;
constexpr int kDownProbesBeforeRestart = 2;
} // namespace

WTrackStarBot::WTrackStarBot(QWidget* pParent, UserSettingsPointer pConfig)
        : WWidget(pParent),
          m_pConfig(pConfig),
          m_pStack(new QStackedLayout(this)),
          m_pNote(new QLabel(this)) {
    m_pStack->setContentsMargins(0, 0, 0, 0);
    m_pNote->setObjectName(QStringLiteral("TrackStarBotNote"));
    m_pNote->setAlignment(Qt::AlignCenter);
    m_pNote->setWordWrap(true);
    m_pNote->setText(tr("TrackStar DJ Bot is starting…"));
    m_pStack->addWidget(m_pNote);

    m_probeTimer.setInterval(kProbeIntervalMs);
    connect(&m_probeTimer, &QTimer::timeout, this, &WTrackStarBot::slotProbe);
    m_probeTimer.start();
    slotProbe();
}

WTrackStarBot::~WTrackStarBot() {
    m_probeTimer.stop();
    if (m_pProbe) {
        m_pProbe->abort();
    }
    // The container owns the foreign QWindow, which does not own the native view: release it after.
    delete m_pContainer;
    m_pContainer = nullptr;
    trackstar::webview::release(m_pWebView);
    m_pWebView = nullptr;
}

void WTrackStarBot::slotProbe() {
    if (m_pProbe) {
        return; // previous probe still running
    }
    QTcpSocket* pProbe = new QTcpSocket(this);
    m_pProbe = pProbe;
    connect(pProbe, &QTcpSocket::connected, this, [this, pProbe]() {
        pProbe->abort();
        pProbe->deleteLater();
        setServiceUp(true);
    });
    connect(pProbe, &QTcpSocket::errorOccurred, this, [this, pProbe]() {
        pProbe->deleteLater();
        setServiceUp(false);
    });
    pProbe->connectToHost(QStringLiteral("127.0.0.1"),
            static_cast<quint16>(trackstar::kBotServicePort));
}

void WTrackStarBot::setServiceUp(bool up) {
    if (up) {
        m_downProbes = 0;
        if (!m_serviceUp) {
            m_serviceUp = true;
            loadPage();
        }
        return;
    }
    ++m_downProbes;
    if (m_serviceUp && m_downProbes >= kDownProbesBeforeRestart) {
        m_serviceUp = false;
        m_pStack->setCurrentWidget(m_pNote);
        m_pNote->setText(tr("TrackStar DJ Bot stopped — restarting…"));
    }
    if (!m_serviceUp && m_downProbes >= kDownProbesBeforeRestart) {
        if (!trackstar::ensureBotService(m_pConfig)) {
            m_pNote->setText(
                    tr("TrackStar DJ Bot is not installed.\n"
                       "Install TrackStar DJ Bot, or show the library with the LIBRARY "
                       "button."));
        }
    }
}

void WTrackStarBot::createWebView() {
    if (m_pContainer) {
        return;
    }
    m_pWebView = trackstar::webview::create();
    if (!m_pWebView) {
        m_pNote->setText(tr("The Bot panel needs macOS for now."));
        return;
    }
    QWindow* pWindow = QWindow::fromWinId(trackstar::webview::nativeId(m_pWebView));
    m_pContainer = QWidget::createWindowContainer(pWindow, this);
    m_pContainer->setObjectName(QStringLiteral("TrackStarBotView"));
    m_pContainer->setFocusPolicy(Qt::ClickFocus);
    m_pStack->addWidget(m_pContainer);
}

void WTrackStarBot::loadPage() {
    createWebView();
    if (!m_pContainer) {
        return;
    }
    trackstar::webview::load(m_pWebView, trackstar::botServiceUrl());
    m_pStack->setCurrentWidget(m_pContainer);
}
