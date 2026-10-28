// XSEngine.mm —— 无 hook 扫拍引擎（iOS 18 arm64e 上代码页被 TXM 保护，Dobby 装不了）
//
// 思路：完全不改游戏代码，只「拿对象 + 调方法」：
//   1. 用 il2cpp 按名字找到 CeraAuctionListPageBase
//   2. 用 UnityEngine.Object.FindObjectOfType(Type) 拿到当前页面实例（不需要 hook 抓）
//   3. 自己的定时器（主队列）按间隔调 OnClickRefresh / OnClickSearchButton / OnClickEnter_Search
//
// 拿不到的东西：解析/扫到/已购（那些必须 hook 或轮询），先给 0。
#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>
#include <dlfcn.h>
#include <string.h>
#include <time.h>
#include <atomic>
#include "platform_ios.h"
#include "XSAPI.h"

/* ---------- il2cpp 绑定 ---------- */
static void* (*p_domain_get)(void);
static void* (*p_domain_get_assemblies)(void*, size_t*);
static void* (*p_assembly_get_image)(void*);
static const char* (*p_image_get_name)(void*);
static void* (*p_class_from_name)(void*, const char*, const char*);
static void* (*p_class_get_method_from_name)(void*, const char*, int);
static void* (*p_method_get_param_count)(void*);
static void* (*p_class_get_type)(void*);
static void* (*p_type_get_object)(void*);
static void* (*p_runtime_invoke)(void*, void*, void**, void**);
static void* (*p_string_new)(void*, const char*);
static void* (*p_object_new)(void*);

static bool g_eng_ready = false;
static void* g_img_csharp = NULL;
static void* g_cls_page = NULL;
static void* g_cls_dc = NULL;

static std::atomic<int> g_calls{0};
static std::atomic<int> g_interval_ms{1100};
static std::atomic<bool> g_running{false};
static std::atomic<bool> g_mode_btn{true};
static std::atomic<bool> g_mode_search{true};
static std::atomic<bool> g_mode_sort{false};
static std::atomic<bool> g_mode_price{false};
static dispatch_source_t g_timer = nil;
static int64_t g_last_tick_ms = 0;
static std::atomic<int> g_last_gap{0};

static int64_t xs_now_ms(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void* xs_sym(const char* name) {
    void* p = elf_find_sym(maps_first_base("UnityFramework"), name);
    return p;
}

extern "C" int xs_engine_init(void) {
    @try {
        if (g_eng_ready) return 1;
        xs_crash_note("engine", "init start");
        p_domain_get              = (void*(*)(void))xs_sym("il2cpp_domain_get");
        p_domain_get_assemblies   = (void*(*)(void*,size_t*))xs_sym("il2cpp_domain_get_assemblies");
        p_assembly_get_image      = (void*(*)(void*))xs_sym("il2cpp_assembly_get_image");
        p_image_get_name          = (const char*(*)(void*))xs_sym("il2cpp_image_get_name");
        p_class_from_name         = (void*(*)(void*,const char*,const char*))xs_sym("il2cpp_class_from_name");
        p_class_get_method_from_name = (void*(*)(void*,const char*,int))xs_sym("il2cpp_class_get_method_from_name");
        p_class_get_type          = (void*(*)(void*))xs_sym("il2cpp_class_get_type");
        p_type_get_object         = (void*(*)(void*))xs_sym("il2cpp_type_get_object");
        p_runtime_invoke          = (void*(*)(void*,void*,void**,void**))xs_sym("il2cpp_runtime_invoke");
        p_string_new              = (void*(*)(void*,const char*))xs_sym("il2cpp_string_new");
        p_object_new              = (void*(*)(void*))xs_sym("il2cpp_object_new");

        if (!p_domain_get || !p_class_from_name || !p_domain_get_assemblies) {
            xs_crash_note("engine", "il2cpp api missing");
            return 0;
        }
        void* dom = p_domain_get();
        if (!dom) { xs_crash_note("engine", "domain null"); return 0; }

        size_t n = 0;
        void** asms = (void**)p_domain_get_assemblies(dom, &n);
        for (size_t i = 0; i < n && i < 400; i++) {
            void* img = p_assembly_get_image(asms[i]);
            if (!img) continue;
            const char* nm = p_image_get_name(img);
            if (nm && strstr(nm, "Assembly-CSharp") && !strstr(nm, "firstpass")) { g_img_csharp = img; break; }
        }
        if (!g_img_csharp) { xs_crash_note("engine", "Assembly-CSharp not found"); return 0; }
        xs_crash_note("engine", p_image_get_name(g_img_csharp));

        g_cls_page = p_class_from_name(g_img_csharp, "", "CeraAuctionListPageBase");
        if (!g_cls_page) g_cls_page = p_class_from_name(g_img_csharp, "MDNF.Auction.Cera", "CeraAuctionListPageBase");
        g_cls_dc = p_class_from_name(g_img_csharp, "MDNF.Auction.Cera", "CeraAuctionDataController");

        char b[160];
        snprintf(b, sizeof(b), "page=%p dc=%p", g_cls_page, g_cls_dc);
        xs_crash_note("engine", b);
        if (!g_cls_page) return 0;

        g_eng_ready = true;
        return 1;
    } @catch (...) { return 0; }
}

extern "C" int xs_engine_ready(void) { return g_eng_ready ? 1 : 0; }

/* 用 UnityEngine.Object.FindObjectOfType(Type) 拿页面实例 */
static void* xs_find_page_instance(void) {
    if (!g_cls_page || !p_class_get_type || !p_type_get_object || !p_runtime_invoke) return NULL;
    void* dom = p_domain_get();
    size_t n = 0; void** asms = (void**)p_domain_get_assemblies(dom, &n);
    void* coreImg = NULL;
    for (size_t i = 0; i < n && i < 400; i++) {
        void* img = p_assembly_get_image(asms[i]); if (!img) continue;
        const char* nm = p_image_get_name(img);
        if (nm && strstr(nm, "UnityEngine.CoreModule")) { coreImg = img; break; }
    }
    if (!coreImg) return NULL;
    void* objCls = p_class_from_name(coreImg, "UnityEngine", "Object");
    if (!objCls) return NULL;
    void* m = p_class_get_method_from_name(objCls, "FindObjectOfType", 1);
    if (!m) return NULL;
    void* typeObj = p_type_get_object(p_class_get_type(g_cls_page));
    if (!typeObj) return NULL;
    void* args[1] = { typeObj };
    void* exc = NULL;
    return p_runtime_invoke(m, NULL, args, &exc);
}

static int xs_call0(void* cls, void* inst, const char* name) {
    void* mi = p_class_get_method_from_name(cls, name, 0);
    if (!mi) return -1;
    void* fn = *(void**)mi;
    if (!fn) return -2;
    ((void(*)(void*))fn)(inst);
    return 0;
}

static void xs_tick(void) {
    if (!g_running.load()) return;
    @autoreleasepool {
        int64_t now = xs_now_ms();
        if (g_last_tick_ms) g_last_gap.store((int)(now - g_last_tick_ms));
        g_last_tick_ms = now;

        void* page = xs_find_page_instance();
        if (!page) { xs_crash_note("engine", "no page instance"); return; }

        if (g_mode_btn.load())    xs_call0(g_cls_page, page, "OnClickRefresh");
        if (g_mode_search.load()) {
            if (xs_call0(g_cls_page, page, "OnClickSearchButton") != 0)
                xs_call0(g_cls_page, page, "OnClickEnter_Search");
        }
        g_calls.fetch_add(1);
    }
}

extern "C" void xs_engine_set_interval(int ms) {
    if (ms < 100) ms = 100;
    if (ms > 60000) ms = 60000;
    g_interval_ms.store(ms);
}

extern "C" void xs_engine_set_modes(int btn, int search, int sort, int price) {
    g_mode_btn.store(btn != 0);
    g_mode_search.store(search != 0);
    g_mode_sort.store(sort != 0);
    g_mode_price.store(price != 0);
}

extern "C" void xs_engine_start(void) {
    if (!g_eng_ready && !xs_engine_init()) return;
    dispatch_async(dispatch_get_main_queue(), ^{
        if (g_timer) { dispatch_source_cancel(g_timer); g_timer = nil; }
        g_running.store(true);
        uint64_t iv = (uint64_t)g_interval_ms.load() * NSEC_PER_MSEC;
        g_timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_main_queue());
        dispatch_source_set_timer(g_timer, dispatch_time(DISPATCH_TIME_NOW, iv), iv, iv / 10);
        dispatch_source_set_event_handler(g_timer, ^{ xs_tick(); });
        dispatch_resume(g_timer);
        xs_crash_note("engine", "started");
    });
}

extern "C" void xs_engine_stop(void) {
    dispatch_async(dispatch_get_main_queue(), ^{
        g_running.store(false);
        if (g_timer) { dispatch_source_cancel(g_timer); g_timer = nil; }
        xs_crash_note("engine", "stopped");
    });
}

extern "C" int xs_engine_calls(void) { return g_calls.load(); }
extern "C" int xs_engine_gap(void) { return g_last_gap.load(); }
