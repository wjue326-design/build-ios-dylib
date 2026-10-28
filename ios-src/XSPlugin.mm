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
    xs_crash_note("init", "UnityFramework found, reloading il2cpp api");
    xs_reload_api();
    xs_crash_note("init", "api reloaded, calling dnf_scan_init");
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
            o["data"] = { {"diag", xs_engine_ready() ? "engine v" : "engine x"}, {"ready", xs_engine_ready() == 1} };
        }
        else if (cmd == "cfg") {
            json d = req.value("data", json::object());
            int ivms = (int)d.value("interval", 1100.0);
            xs_engine_set_interval(ivms);
            dnf_auto_on_set_ms(d.value("delay", 100));
            json m = d.value("mode", json::object());
            xs_engine_set_modes(m.value("btn", true), m.value("search", true), m.value("sort", false), m.value("price", false));
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
        else if (cmd == "initKernel") {
            int ok = xs_engine_init();
            xs_crash_note("engine", ok ? "init ok" : "init fail");
            o["data"] = {{"ready", ok == 1}, {"diag", ok ? "engine v" : "engine x"}};
        }
        else if (cmd == "start") { xs_engine_start(); o["data"] = {{"started", true}}; }
        else if (cmd == "stop")  { xs_engine_stop(); o["data"] = {{"stopped", true}}; }
        else if (cmd == "stats") {
            o["data"] = {
                {"seen",   0},
                {"bought", 0},
                {"calls",  xs_engine_calls()},
                {"parse",  0},
                {"cap",    0},
                {"det",    0},
                {"gap",    xs_engine_gap()},
                {"page",   dnf_rs_page_found() == 1},
                {"P",      xs_engine_ready() == 1},
                {"D",      false},
                {"diag",   std::string(xs_engine_ready() ? "engine v" : "engine x") + " | " + xs_engine_state()},
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
        else if (cmd == "probe") {
            char* d = xs_engine_probe();
            o["data"] = { {"dump", d ? std::string(d) : std::string("null")} };
            if (d) free(d);
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
