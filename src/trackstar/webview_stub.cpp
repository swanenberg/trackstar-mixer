#include "trackstar/webview.h"

// No native web view outside macOS yet (Windows: WebView2 later).
namespace trackstar::webview {

void* create() {
    return nullptr;
}

WId nativeId(void*) {
    return 0;
}

void load(void*, const QString&) {
}

void release(void*) {
}

} // namespace trackstar::webview
