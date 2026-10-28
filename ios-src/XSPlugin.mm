// XSPlugin.mm —— H5GG 桥（loadPlugin 用）+ 统一 JSON 入口的实现
#import <Foundation/Foundation.h>
#include <pthread.h>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include "platform_ios.h"
#include "XSAPI.h"
#include "json.hpp"

using nlohmann::json;

static std::atomic<int> g_started{0};

static std::string diag_string(void){
    int b = dnf_diag_bits();
    auto bit = [&](int i){ return (b & (1 << i)) ? "v" : "x"; };
    char out[64];
    snprintf(out, sizeof(out), "P%s C%s U%s R%s HU%s HR%s I%s",
             bit(0), bit(1), bit(2), bit(3), bit(4), bit(5), bit(6));
    return out;
}

static void* start_thread(void*){
    xs_crash_note("init", "waiting UnityFramework");
    for (int i = 0; i < 600 && !maps_first_base("UnityFramework"); i++) usleep(200 * 1000);
    xs_crash_note("init", "UnityFramework found, calling dnf_scan_init");
    dnf_scan_init();
    g_started.store(1);
    xs_crash_note("init", diag_string().c_str());
    return NULL;
}

static void ensure_started(void){
    static std::atomic<int> once{0};
    if (once.exchange(1)) return;
    pthread_t t; pthread_create(&t, NULL, start_thread, NULL); pthread_detach(t);
}

/* ---------------- 统一 JSON 入口 ---------------- */
extern "C" char* xs_api_call(const char* reqUtf8){
    std::string out;
    try {
        json req = json::parse(std::string(reqUtf8 ? reqUtf8 : "{}"));
        std::string cmd = req.value("cmd", "");
        json o; o["ok"] = true;

        if (cmd == "init") {
            o["data"] = { {"diag", diag_string()}, {"ready", g_started.load() == 1} };
        }
        else if (cmd == "cfg") {
            json d = req.value("data", json::object());
            dnf_scan_set_interval_ms(d.value("interval", 1100.0));
            dnf_auto_on_set_ms(d.value("delay", 100));
            json m = d.value("mode", json::object());
            dnf_use_click_set(m.value("btn", true) ? 1 : 0);
            dnf_use_search_set(m.value("search", true) ? 1 : 0);
            dnf_sort_sync_set(m.value("sort", false) ? 1 : 0);
            dnf_use_price_set(m.value("price", false) ? 1 : 0);
            int lim = d.value("limit", 0);
            if (lim > 0) { dnf_set_limit(lim); dnf_limit_set(1); } else dnf_limit_set(0);
            int buy = d.value("buy", false) ? 1 : 0;
            dnf_buy2_set(buy);
            dnf_tip_set(d.value("tip", false) ? 1 : 0);
            dnf_log_set(d.value("log", false) ? 1 : 0);
            o["data"] = {{"applied", true}};
        }
        else if (cmd == "initKernel") { ensure_started(); o["data"] = {{"started", true}}; }
        else if (cmd == "start") { dnf_scan_set_enabled(1); o["data"] = {{"started", true}}; }
        else if (cmd == "stop")  { dnf_scan_set_enabled(0); dnf_buy2_set(0); o["data"] = {{"stopped", true}}; }
        else if (cmd == "stats") {
            o["data"] = {
                {"seen",   dnf_seen_count()},
                {"bought", dnf_buy_count()},
                {"calls",  dnf_scan_count()},
                {"parse",  dnf_parse_cnt()},
                {"cap",    dnf_rs_capture_cnt()},
                {"det",    dnf_rs_detail_cnt()},
                {"gap",    dnf_actual_gap_ms()},
                {"page",   dnf_rs_page_found() == 1},
                {"P",      dnf_rs_page_found() == 1},
                {"D",      dnf_rs_detail_valid() == 1},
                {"diag",   diag_string()},
                {"mode",   {{"btn", dnf_use_click_get() == 1},
                            {"search", dnf_use_search_get() == 1},
                            {"sort", dnf_sort_sync_get() == 1},
                            {"price", dnf_use_price_get() == 1}}}
            };
        }
        else if (cmd == "limitOn") {
            json d = req.value("data", json::object());
            bool on = d.value("on", false);
            dnf_set_limit(d.value("num", 10));
            dnf_limit_set(on ? 1 : 0);
            o["data"] = {{"on", on}};
        }
        else if (cmd == "clearCap") { dnf_rs_reset(); o["data"] = {{"ok", true}}; }
        else if (cmd == "calibSort")  { dnf_snap_time();  o["data"] = {{"field", dnf_get_field_time()}}; }
        else if (cmd == "calibPrice") { dnf_snap_price(); o["data"] = {{"field", dnf_get_field_price()}}; }
        else if (cmd == "log") { NSLog(@"[XS] %s", req.value("data", "").c_str()); }
        else { o = {{"ok", false}, {"err", "unknown-cmd"}}; }

        out = o.dump();
    } catch (...) {
        out = R"({"ok":false,"err":"exception"})";
    }
    char* r = (char*)malloc(out.size() + 1);
    memcpy(r, out.c_str(), out.size() + 1);
    return r;   /* 调用方 free */
}

/* ---------------- H5GG 用的 OC 实例 ---------------- */
@interface XSPlugin : NSObject
- (NSString *)api:(NSString *)jsonStr;
@end

@implementation XSPlugin
- (instancetype)init { self = [super init]; return self; }
- (NSString *)api:(NSString *)jsonStr {
    char* r = xs_api_call([jsonStr UTF8String]);
    NSString* s = [NSString stringWithUTF8String:(r ? r : "{\"ok\":false}")];
    if (r) free(r);
    return s ?: @"{\"ok\":false}";
}
@end

/* 内核 hook 改为手动触发：面板点「扫拍刷新」时才 initKernel（避免进场就装 44 个 hook） */
