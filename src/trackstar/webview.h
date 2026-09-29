#pragma once

// Thin native web view for WTrackStarBot. macOS: WKWebView (system WebKit, no QtWebEngine needed);
// other platforms: not available (create() returns nullptr, the widget shows a note).

#include <QString>
#include <QWindow>

namespace trackstar::webview {

/// A new web view (retained), or nullptr when the platform has none.
void* create();
/// The native view handle for QWindow::fromWinId.
WId nativeId(void* pView);
void load(void* pView, const QString& url);
void release(void* pView);

} // namespace trackstar::webview
