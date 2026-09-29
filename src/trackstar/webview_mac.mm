#include "trackstar/webview.h"

#import <AppKit/AppKit.h>
#import <WebKit/WebKit.h>

namespace trackstar::webview {

void* create() {
    WKWebViewConfiguration* config = [[WKWebViewConfiguration alloc] init];
    config.websiteDataStore = [WKWebsiteDataStore nonPersistentDataStore];
    WKWebView* view = [[WKWebView alloc] initWithFrame:NSMakeRect(0, 0, 640, 400)
                                         configuration:config];
    // The page paints its own background; no white flash while it loads.
    [view setValue:@NO forKey:@"drawsBackground"];
    if (@available(macOS 12.0, *)) {
        view.underPageBackgroundColor = [NSColor colorWithSRGBRed:0x0B / 255.0
                                                            green:0x0D / 255.0
                                                             blue:0x14 / 255.0
                                                            alpha:1.0];
    }
    if (@available(macOS 13.3, *)) {
        view.inspectable = YES; // Safari → Develop → TrackStar DJ Mixer
    }
    view.allowsMagnification = NO;
    return (__bridge_retained void*)view;
}

WId nativeId(void* pView) {
    return reinterpret_cast<WId>(pView);
}

void load(void* pView, const QString& url) {
    if (!pView) {
        return;
    }
    WKWebView* view = (__bridge WKWebView*)pView;
    NSURL* nsUrl = [NSURL URLWithString:url.toNSString()];
    [view loadRequest:[NSURLRequest requestWithURL:nsUrl
                                       cachePolicy:NSURLRequestReloadIgnoringLocalCacheData
                                   timeoutInterval:10]];
}

void release(void* pView) {
    if (!pView) {
        return;
    }
    WKWebView* view = (__bridge_transfer WKWebView*)pView;
    [view stopLoading];
    view = nil;
}

} // namespace trackstar::webview
