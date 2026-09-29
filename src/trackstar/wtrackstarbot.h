#pragma once

// WTrackStarBot: the Bot inside the Mixer. A skin element (<TrackStarBot>) that shows the Bot helper's
// page (see botservice.h) in a native WKWebView on macOS, in the spot where the skin puts it (in
// TangoBigBPM: where the library menu and track list were). Until the helper answers it shows a note
// and (re)starts the helper; when the helper goes away it falls back to the note and reloads later.

#include <QPointer>
#include <QTimer>

#include "preferences/usersettings.h"
#include "widget/wwidget.h"

class QLabel;
class QStackedLayout;
class QTcpSocket;

class WTrackStarBot : public WWidget {
    Q_OBJECT
  public:
    WTrackStarBot(QWidget* pParent, UserSettingsPointer pConfig);
    ~WTrackStarBot() override;

  private slots:
    void slotProbe();

  private:
    void setServiceUp(bool up);
    void createWebView();
    void loadPage();

    UserSettingsPointer m_pConfig;
    QStackedLayout* m_pStack;
    QLabel* m_pNote;
    QWidget* m_pContainer = nullptr;
    void* m_pWebView = nullptr; // WKWebView*, retained (bridged)
    QTimer m_probeTimer;
    QPointer<QTcpSocket> m_pProbe;
    bool m_serviceUp = false;
    int m_downProbes = 0;
};
