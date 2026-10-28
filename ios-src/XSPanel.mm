// XSPanel.mm v4 —— 强重试 + 全链路日志 + 加载失败兜底
#import <UIKit/UIKit.h>
#import <WebKit/WebKit.h>
#include "XSAPI.h"

#if __has_include("XSHTML.h")
#include "XSHTML.h"
#define XS_HAS_EMBEDDED_HTML 1
#else
#define XS_HAS_EMBEDDED_HTML 0
#endif

static UIView*      g_panel = nil;
static WKWebView*   g_web   = nil;
static UIButton*    g_ball  = nil;
static BOOL         g_collapsed = NO;
static CGRect       g_fullRect = {{34, 56}, {440, 620}};
static NSTimer*     g_statsTimer = nil;
static int          g_tries = 0;
static NSTimer*     g_retryTimer = nil;

static NSString* const kShim =
    @"window.__XS_NATIVE__ = true;"
     "window.__xs_post = function(o){ try { window.webkit.messageHandlers.xs.postMessage(o); } catch(e){} };"
     "window.__xs_on_result = null;"
     "window.__xs_on_stats = null;"
     "window.onerror = function(m,s,l){ try { window.webkit.messageHandlers.xs.postMessage({id:0, req:{cmd:'__jserr', msg:String(m)+' @'+l}}); } catch(e){} };";

@interface XSPanel : NSObject <WKScriptMessageHandler, WKNavigationDelegate>
+ (instancetype)shared;
- (void)attach;
- (void)scheduleRetry;
- (void)collapse:(BOOL)on;
@end

static UIWindow* xs_keyWindow(void){
    for (UIWindow* w in UIApplication.sharedApplication.windows) if (w.isKeyWindow) return w;
    for (UIWindow* w in UIApplication.sharedApplication.windows) if (!w.hidden) return w;
    return UIApplication.sharedApplication.windows.firstObject;
}

@implementation XSPanel
+ (instancetype)shared { static XSPanel* s; static dispatch_once_t o; dispatch_once(&o, ^{ s = [XSPanel new]; }); return s; }

- (void)scheduleRetry {
    if (g_panel) return;
    if (g_tries++ > 60) { xs_crash_note("panel", "give up: no keyWindow after 60s"); return; }
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(1.0*NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
        [self attach];
    });
}

- (void)attach {
    @try {
        if (g_panel) return;
        xs_crash_note("panel", "attach try");
        UIWindow* host = xs_keyWindow();
        if (!host) { xs_crash_note("panel", "no keyWindow, retry"); [self scheduleRetry]; return; }

        WKWebViewConfiguration* cfg = [WKWebViewConfiguration new];
        WKUserContentController* ucc = [WKUserContentController new];
        [ucc addUserScript:[[WKUserScript alloc] initWithSource:kShim injectionTime:WKUserScriptInjectionTimeAtDocumentStart forMainFrameOnly:YES]];
        [ucc addScriptMessageHandler:self name:@"xs"];
        cfg.userContentController = ucc;

        g_panel = [[UIView alloc] initWithFrame:g_fullRect];
        g_panel.backgroundColor = [UIColor colorWithRed:0.09 green:0.09 blue:0.11 alpha:0.97];  /* 兜底可见 */
        g_panel.layer.cornerRadius = 10;
        g_panel.layer.masksToBounds = YES;

        g_web = [[WKWebView alloc] initWithFrame:g_panel.bounds configuration:cfg];
        g_web.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
        g_web.backgroundColor = UIColor.clearColor;
        g_web.opaque = NO;
        g_web.navigationDelegate = self;
        g_web.scrollView.bounces = NO;
        if (@available(iOS 11.0, *)) g_web.scrollView.contentInsetAdjustmentBehavior = UIScrollViewContentInsetAdjustmentNever;
        [g_panel addSubview:g_web];

        g_ball = [UIButton buttonWithType:UIButtonTypeSystem];
        g_ball.frame = CGRectMake(0, 0, 56, 56);
        g_ball.backgroundColor = [UIColor colorWithRed:0.31 green:0.55 blue:1.0 alpha:0.95];
        g_ball.layer.cornerRadius = 10;
        [g_ball setTitle:@"扫" forState:UIControlStateNormal];
        [g_ball setTitleColor:UIColor.whiteColor forState:UIControlStateNormal];
        g_ball.titleLabel.font = [UIFont boldSystemFontOfSize:20];
        [g_ball addTarget:self action:@selector(onBallTap) forControlEvents:UIControlEventTouchUpInside];
        g_ball.hidden = YES;
        [g_panel addSubview:g_ball];

        [host addSubview:g_panel];

        UIPanGestureRecognizer* pan = [[UIPanGestureRecognizer alloc] initWithTarget:self action:@selector(onPan:)];
        [g_panel addGestureRecognizer:pan];

        xs_crash_note("panel", [[NSString stringWithFormat:@"attached to %@ frame=%@", NSStringFromClass(host.class), NSStringFromCGRect(host.bounds)] UTF8String]);
        [self loadHTML];
        [self startStats];
        [self kickRenderCheck];
    } @catch (NSException* e) {
        xs_crash_note("panel", [[NSString stringWithFormat:@"attach exception %@", e] UTF8String]);
    }
}

- (void)kickRenderCheck {
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(4.0*NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
        if (!g_web) return;
        NSString* js = @"(function(){ try { return String(document.body && document.body.innerText ? document.body.innerText.length : -1); } catch(e){ return 'err'; } })()";
        [g_web evaluateJavaScript:js completionHandler:^(id r, NSError* e){
            xs_crash_note("panel", [[NSString stringWithFormat:@"rendercheck len=%@ err=%@", r ?: @"nil", e.localizedDescription ?: @"nil"] UTF8String]);
        }];
    });
}

- (void)webView:(WKWebView*)w didFinishNavigation:(WKNavigation*)n { xs_crash_note("panel", "html loaded"); }
- (void)webView:(WKWebView*)w didFailNavigation:(WKNavigation*)n withError:(NSError*)e {
    xs_crash_note("panel", [[NSString stringWithFormat:@"html fail %@", e.localizedDescription] UTF8String]);
}
- (void)webView:(WKWebView*)w didFailProvisionalNavigation:(WKNavigation*)n withError:(NSError*)e {
    xs_crash_note("panel", [[NSString stringWithFormat:@"html failProvisional %@", e.localizedDescription] UTF8String]);
}

- (void)onPan:(UIPanGestureRecognizer*)g {
    CGPoint t = [g translationInView:g_panel.superview];
    if (g.state == UIGestureRecognizerStateChanged) {
        CGPoint c = g_panel.center; c.x += t.x; c.y += t.y; g_panel.center = c;
        [g setTranslation:CGPointZero inView:g_panel.superview];
    } else if (g.state == UIGestureRecognizerStateEnded) {
        g_fullRect = g_panel.frame;
    }
}

- (void)loadHTML {
    NSString* ext = [NSHomeDirectory() stringByAppendingPathComponent:@"Documents/xys/index.html"];
    if ([[NSFileManager defaultManager] fileExistsAtPath:ext]) {
        NSURL* u = [NSURL fileURLWithPath:ext];
        [g_web loadFileURL:u allowingReadAccessToURL:[u URLByDeletingLastPathComponent]];
        xs_crash_note("panel", [[NSString stringWithFormat:@"html=%@", ext] UTF8String]);
        return;
    }
#if XS_HAS_EMBEDDED_HTML
    {
        NSString* html = [[NSString alloc] initWithBytes:XSHTML_DATA length:XSHTML_DATA_len encoding:NSUTF8StringEncoding];
        [g_web loadHTMLString:html baseURL:nil];
        xs_crash_note("panel", [[NSString stringWithFormat:@"html=embedded %lu", (unsigned long)html.length] UTF8String]);
    }
#else
    [g_web loadHTMLString:@"<h3 style='color:#fff'>XS: no html</h3>" baseURL:nil];
    xs_crash_note("panel", "html=missing");
#endif
}

- (void)startStats {
    if (g_statsTimer) return;
    g_statsTimer = [NSTimer scheduledTimerWithTimeInterval:0.2 repeats:YES block:^(NSTimer* t){
        char* r = xs_api_call("{\"cmd\":\"stats\"}");
        if (!r) return;
        NSString* js = [NSString stringWithFormat:@"window.__xs_on_stats && window.__xs_on_stats(%s);", r];
        free(r);
        dispatch_async(dispatch_get_main_queue(), ^{ [g_web evaluateJavaScript:js completionHandler:nil]; });
    }];
}

- (void)collapse:(BOOL)on {
    @try {
        g_collapsed = on;
        if (on) {
            CGRect f = g_panel.frame;
            [UIView animateWithDuration:0.2 animations:^{
                g_panel.frame = CGRectMake(f.origin.x, f.origin.y, 56, 56);
                g_web.alpha = 0;
            } completion:^(BOOL ok){ g_web.hidden = YES; g_ball.hidden = NO; }];
        } else {
            [UIView animateWithDuration:0.2 animations:^{ g_panel.frame = g_fullRect; }
                             completion:^(BOOL ok){ g_web.hidden = NO; g_web.alpha = 1; g_ball.hidden = YES; }];
        }
    } @catch (NSException* e) { xs_crash_note("panel", "collapse exc"); }
}

- (void)onBallTap { [self collapse:NO]; }

- (void)userContentController:(WKUserContentController*)ucc didReceiveScriptMessage:(WKScriptMessage*)msg {
    @try {
        NSDictionary* dict = nil;
        id body = msg.body;
        if ([body isKindOfClass:NSDictionary.class]) dict = body;
        else if ([body isKindOfClass:NSString.class])
            dict = [NSJSONSerialization JSONObjectWithData:[(NSString*)body dataUsingEncoding:NSUTF8StringEncoding] options:0 error:nil];
        if (!dict) return;
        NSNumber* mid = dict[@"id"];
        id req = dict[@"req"] ?: @{};
        NSString* cmd = [req isKindOfClass:NSDictionary.class] ? (req[@"cmd"] ?: @"") : @"";

        if ([cmd isEqualToString:@"__move"]) {
            CGFloat dx = [req[@"dx"] doubleValue], dy = [req[@"dy"] doubleValue];
            CGRect f = g_panel.frame; f.origin.x += dx; f.origin.y += dy; g_panel.frame = f;
            if (!g_collapsed) g_fullRect = f;
            return;
        }
        if ([cmd isEqualToString:@"__min"] || [cmd isEqualToString:@"__hide"]) { [self collapse:YES]; return; }
        if ([cmd isEqualToString:@"__restore"]) { [self collapse:NO]; return; }
        if ([cmd isEqualToString:@"__jserr"]) { xs_crash_note("jserr", [req[@"msg"] UTF8String]); return; }

        NSData* d = [NSJSONSerialization dataWithJSONObject:req options:0 error:nil];
        NSString* reqStr = [[NSString alloc] initWithData:d encoding:NSUTF8StringEncoding];
        char* r = xs_api_call(reqStr.UTF8String);
        NSString* payload = r ? [NSString stringWithUTF8String:r] : @"{\"ok\":false}";
        if (r) free(r);
        NSString* js = [NSString stringWithFormat:@"window.__xs_on_result && window.__xs_on_result(%@, %@);", mid ?: @0, payload];
        dispatch_async(dispatch_get_main_queue(), ^{ [g_web evaluateJavaScript:js completionHandler:nil]; });
    } @catch (NSException* e) { xs_crash_note("panel", "msg exc"); }
}
@end

extern "C" void xs_panel_show(void){
    dispatch_async(dispatch_get_main_queue(), ^{ @try { [[XSPanel shared] attach]; } @catch (NSException* e) { xs_crash_note("panel", "show exc"); } });
}

__attribute__((constructor)) static void xs_panel_onload(void){
    xs_crash_note("panel", "dylib constructor fired");
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(2.0*NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
        xs_panel_show();
    });
    [[NSNotificationCenter defaultCenter] addObserverForName:UIApplicationDidBecomeActiveNotification
                                                      object:nil queue:[NSOperationQueue mainQueue]
                                                  usingBlock:^(NSNotification* n){
        xs_crash_note("panel", "app did become active");
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(1.0*NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
            xs_panel_show();
        });
    }];
}
