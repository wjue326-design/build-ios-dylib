// XSPanel.mm —— 单文件方案：dylib 自带 UIKit 悬浮窗 + WKWebView 面板
//
// 注入这一个 dylib 就够了，不需要 H5GG：
//   • 进程内建一个 UIWindow（层级 Alert+1），里面一个透明 WKWebView
//   • HTML 优先读 /var/mobile/Documents/xys/index.html（改 UI 不用重编）
//   • 没有外部文件就用编译时内嵌的那一份（XSHTML.h）
//   • JS ↔ 原生 走 WKScriptMessageHandler，调 xs_api_call()
#import <UIKit/UIKit.h>
#import <WebKit/WebKit.h>
#include "XSAPI.h"

#if __has_include("XSHTML.h")
#include "XSHTML.h"
#define XS_HAS_EMBEDDED_HTML 1
#else
#define XS_HAS_EMBEDDED_HTML 0
#endif

static UIWindow* g_win = nil;
static WKWebView* g_web = nil;
static UIView* g_ball = nil;
static CGRect g_rect = {{34, 56}, {440, 620}};
static NSTimer* g_statsTimer = nil;
static id g_handler = nil;

/* ---------------- 内嵌 shim（注入到页面最前面） ---------------- */
static NSString* const kShim =
    @"window.__XS_NATIVE__ = true;"
     "window.__xs_post = function(o){ try { window.webkit.messageHandlers.xs.postMessage(o); } catch(e){} };"
     "window.__xs_on_result = null;"
     "window.__xs_on_stats = null;";

@interface XSPanel : NSObject <WKScriptMessageHandler, WKNavigationDelegate>
+ (instancetype)shared;
- (void)show;
- (void)hideToBall;
- (void)restoreFromBall;
@end

@implementation XSPanel
+ (instancetype)shared {
    static XSPanel* s = nil;
    static dispatch_once_t once;
    dispatch_once(&once, ^{ s = [XSPanel new]; });
    return s;
}

- (UIWindowScene*)activeScene {
    for (UIScene* sc in UIApplication.sharedApplication.connectedScenes) {
        if ([sc isKindOfClass:UIWindowScene.class] &&
            sc.activationState == UISceneActivationStateForegroundActive) return (UIWindowScene*)sc;
    }
    for (UIScene* sc in UIApplication.sharedApplication.connectedScenes) {
        if ([sc isKindOfClass:UIWindowScene.class]) return (UIWindowScene*)sc;
    }
    return nil;
}

- (void)show {
    if (g_win) { g_win.hidden = NO; return; }
    UIWindowScene* scene = [self activeScene];
    if (!scene) return;

    g_win = [[UIWindow alloc] initWithWindowScene:scene];
    g_win.frame = g_rect;
    g_win.windowLevel = UIWindowLevelAlert + 1;
    g_win.backgroundColor = UIColor.clearColor;
    g_win.rootViewController = [UIViewController new];
    g_win.rootViewController.view.backgroundColor = UIColor.clearColor;

    WKWebViewConfiguration* cfg = [WKWebViewConfiguration new];
    WKUserContentController* ucc = [WKUserContentController new];
    [ucc addUserScript:[[WKUserScript alloc] initWithSource:kShim
                                              injectionTime:WKUserScriptInjectionTimeAtDocumentStart
                                           forMainFrameOnly:YES]];
    [ucc addScriptMessageHandler:self name:@"xs"];
    cfg.userContentController = ucc;
    cfg.preferences.javaScriptEnabled = YES;

    g_web = [[WKWebView alloc] initWithFrame:g_win.rootViewController.view.bounds configuration:cfg];
    g_web.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    g_web.backgroundColor = UIColor.clearColor;
    g_web.opaque = NO;
    g_web.scrollView.bounces = NO;
    g_web.scrollView.backgroundColor = UIColor.clearColor;
    g_web.navigationDelegate = self;
    g_web.layer.cornerRadius = 10;
    g_web.layer.masksToBounds = YES;
    if (@available(iOS 11.0, *)) g_web.scrollView.contentInsetAdjustmentBehavior = UIScrollViewContentInsetAdjustmentNever;

    [g_win.rootViewController.view addSubview:g_web];
    g_win.hidden = NO;

    [self loadHTML];
    [self startStatsTimer];
}

- (void)loadHTML {
    NSString* ext = [NSHomeDirectory() stringByAppendingPathComponent:@"Documents/xys/index.html"];
    if ([[NSFileManager defaultManager] fileExistsAtPath:ext]) {
        NSURL* u = [NSURL fileURLWithPath:ext];
        [g_web loadFileURL:u allowingReadAccessToURL:[u URLByDeletingLastPathComponent]];
        NSLog(@"[XS] panel html = %@", ext);
        return;
    }
#if XS_HAS_EMBEDDED_HTML
    NSString* html = [[NSString alloc] initWithBytes:XSHTML_DATA
                                             length:XSHTML_DATA_len
                                           encoding:NSUTF8StringEncoding];
    [g_web loadHTMLString:html baseURL:nil];
    NSLog(@"[XS] panel html = embedded (%lu bytes)", (unsigned long)html.length);
#else
    [g_web loadHTMLString:@"<h3 style='color:#fff;font:14px -apple-system;padding:20px'>"
                          "XS: 没有内嵌 HTML，也没找到 Documents/xys/index.html</h3>" baseURL:nil];
#endif
}

- (void)startStatsTimer {
    if (g_statsTimer) return;
    g_statsTimer = [NSTimer scheduledTimerWithTimeInterval:0.2 repeats:YES block:^(NSTimer* t){
        [self pushStats];
    }];
}

- (void)pushStats {
    char* r = xs_api_call("{\"cmd\":\"stats\"}");
    if (!r) return;
    NSString* js = [NSString stringWithFormat:@"window.__xs_on_stats && window.__xs_on_stats(%s);", r];
    free(r);
    dispatch_async(dispatch_get_main_queue(), ^{ [g_web evaluateJavaScript:js completionHandler:nil]; });
}

/* ---------------- JS → 原生 ---------------- */
- (void)userContentController:(WKUserContentController*)ucc didReceiveScriptMessage:(WKScriptMessage*)msg {
    id body = msg.body;
    NSDictionary* dict = nil;
    if ([body isKindOfClass:NSDictionary.class]) dict = body;
    else if ([body isKindOfClass:NSString.class]) {
        NSData* d = [(NSString*)body dataUsingEncoding:NSUTF8StringEncoding];
        dict = [NSJSONSerialization JSONObjectWithData:d options:0 error:nil];
    }
    if (!dict) return;

    NSNumber* mid = dict[@"id"];
    id req = dict[@"req"] ?: @{};
    NSString* cmd = [req isKindOfClass:NSDictionary.class] ? (req[@"cmd"] ?: @"") : @"";

    /* 面板自身操作，不进内核 */
    if ([cmd isEqualToString:@"__move"]) {
        CGFloat dx = [req[@"dx"] doubleValue], dy = [req[@"dy"] doubleValue];
        CGRect f = g_win.frame; f.origin.x += dx; f.origin.y += dy;
        g_win.frame = f; g_rect = f;
        return;
    }
    if ([cmd isEqualToString:@"__hide"]) { [self hideToBall]; return; }

    NSData* d = [NSJSONSerialization dataWithJSONObject:req options:0 error:nil];
    NSString* reqStr = [[NSString alloc] initWithData:d encoding:NSUTF8StringEncoding];
    char* r = xs_api_call(reqStr.UTF8String);
    NSString* payload = r ? [NSString stringWithUTF8String:r] : @"{\"ok\":false}";
    if (r) free(r);

    NSString* js = [NSString stringWithFormat:@"window.__xs_on_result && window.__xs_on_result(%@, %@);",
                    mid ?: @0, payload];
    dispatch_async(dispatch_get_main_queue(), ^{ [g_web evaluateJavaScript:js completionHandler:nil]; });
}

/* ---------------- 收起成小球 ---------------- */
- (void)hideToBall {
    g_win.hidden = YES;
    if (g_ball) { g_ball.hidden = NO; return; }
    UIWindowScene* scene = [self activeScene];
    if (!scene) return;
    UIWindow* bw = [[UIWindow alloc] initWithWindowScene:scene];
    bw.frame = CGRectMake(g_rect.origin.x + g_rect.size.width - 56, g_rect.origin.y + 8, 44, 44);
    bw.windowLevel = UIWindowLevelAlert + 2;
    bw.backgroundColor = UIColor.clearColor;
    bw.rootViewController = [UIViewController new];
    UIButton* b = [UIButton buttonWithType:UIButtonTypeSystem];
    b.frame = CGRectMake(0, 0, 44, 44);
    b.backgroundColor = [UIColor colorWithRed:0.31 green:0.55 blue:1.0 alpha:0.85];
    b.layer.cornerRadius = 22;
    [b setTitle:@"扫" forState:UIControlStateNormal];
    [b setTitleColor:UIColor.whiteColor forState:UIControlStateNormal];
    [b addTarget:self action:@selector(onBallTap) forControlEvents:UIControlEventTouchUpInside];
    [bw.rootViewController.view addSubview:b];
    bw.hidden = NO;
    g_ball = bw;
}

- (void)onBallTap {
    g_ball.hidden = YES;
    g_win.hidden = NO;
}

@end

extern "C" void xs_panel_show(void){
    dispatch_async(dispatch_get_main_queue(), ^{
        [[XSPanel shared] show];
    });
}

/* 进程一起来就把面板挂上（不等用户操作） */
__attribute__((constructor)) static void xs_panel_onload(void){
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(1.5 * NSEC_PER_SEC)),
                   dispatch_get_main_queue(), ^{
        xs_panel_show();
    });
}
