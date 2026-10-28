// XSEngine.mm —— 无 hook 扫拍引擎（v2：加多路找实例 + 状态诊断）
#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>
#include <dlfcn.h>
#include <string.h>
#include <time.h>
#include <atomic>
#include <stdio.h>
#include "platform_ios.h"
#include "XSAPI.h"

static void* (*p_domain_get)(void);
static void* (*p_domain_get_assemblies)(void*, size_t*);
static void* (*p_assembly_get_image)(void*);
static const char* (*p_image_get_name)(void*);
static void* (*p_class_from_name)(void*, const char*, const char*);
static void* (*p_class_get_method_from_name)(void*, const char*, int);
static void* (*p_class_get_type)(void*);
static void* (*p_type_get_object)(void*);
static void* (*p_runtime_invoke)(void*, void*, void**, void**);
static void* (*p_string_new)(void*, const char*);
static void* (*p_object_new)(void*);

static bool  g_eng_ready = false;
static void* g_img_csharp = NULL;
static void* g_core_img   = NULL;
static void* g_cls_page   = NULL;
static void* g_cls_dc     = NULL;
static void* g_page_inst  = NULL;

static std::atomic<int>  g_calls{0};
static std::atomic<int>  g_interval_ms{1100};
static std::atomic<bool> g_running{false};
static std::atomic<bool> g_mode_btn{true};
static std::atomic<bool> g_mode_search{true};
static std::atomic<bool> g_mode_sort{false};
static std::atomic<bool> g_mode_price{false};
static dispatch_source_t g_timer = nil;
static int64_t g_last_tick_ms = 0;
static std::atomic<int> g_last_gap{0};
static std::atomic<int> g_fail_ticks{0};
static char g_state[128] = "init";

static int64_t xs_now_ms(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
static void* xs_sym(const char* name) {
    return elf_find_sym(maps_first_base("UnityFramework"), name);
}
static void xs_state(const char* s) {
    snprintf(g_state, sizeof(g_state), "%s", s);
}

extern "C" const char* xs_engine_state(void) { return g_state; }

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

        char b[256];
        snprintf(b, sizeof(b), "sym domain=%p class=%p typeobj=%p invoke=%p",
                 (void*)p_domain_get, (void*)p_class_from_name, (void*)p_type_get_object, (void*)p_runtime_invoke);
        xs_crash_note("engine", b);

        if (!p_domain_get || !p_class_from_name || !p_domain_get_assemblies) { xs_state("api missing"); return 0; }
        void* dom = p_domain_get();
        if (!dom) { xs_state("domain null"); return 0; }

        size_t n = 0;
        void** asms = (void**)p_domain_get_assemblies(dom, &n);
        for (size_t i = 0; i < n && i < 400; i++) {
            void* img = p_assembly_get_image(asms[i]);
            if (!img) continue;
            const char* nm = p_image_get_name(img);
            if (!nm) continue;
            if (!g_img_csharp && strstr(nm, "Assembly-CSharp") && !strstr(nm, "firstpass")) g_img_csharp = img;
            if (!g_core_img && strstr(nm, "UnityEngine.CoreModule")) g_core_img = img;
        }
        snprintf(b, sizeof(b), "img csharp=%p core=%p", g_img_csharp, g_core_img);
        xs_crash_note("engine", b);
        if (!g_img_csharp) { xs_state("no Assembly-CSharp"); return 0; }

        g_cls_page = p_class_from_name(g_img_csharp, "", "CeraAuctionListPageBase");
        if (!g_cls_page) g_cls_page = p_class_from_name(g_img_csharp, "MDNF.Auction.Cera", "CeraAuctionListPageBase");
        g_cls_dc = p_class_from_name(g_img_csharp, "MDNF.Auction.Cera", "CeraAuctionDataController");
        snprintf(b, sizeof(b), "page=%p dc=%p", g_cls_page, g_cls_dc);
        xs_crash_note("engine", b);
        if (!g_cls_page) { xs_state("no page class"); return 0; }

        g_eng_ready = true;
        xs_state("ready");
        return 1;
    } @catch (...) { xs_state("exception"); return 0; }
}

extern "C" int xs_engine_ready(void) { return g_eng_ready ? 1 : 0; }

/* 三种方式找页面实例 */
static void* xs_find_via(void* objCls, const char* mName, void* typeObj) {
    void* m = p_class_get_method_from_name(objCls, mName, 1);
    if (!m) return NULL;
    void* args[1] = { typeObj };
    void* exc = NULL;
    void* r = p_runtime_invoke(m, NULL, args, &exc);
    return exc ? NULL : r;
}

static void* xs_find_objects_all(void* resCls, void* typeObj) {
    void* m = p_class_get_method_from_name(resCls, "FindObjectsOfTypeAll", 1);
    if (!m) return NULL;
    void* args[1] = { typeObj };
    void* exc = NULL;
    void* arr = p_runtime_invoke(m, NULL, args, &exc);
    if (!arr || exc) return NULL;
    int64_t len = *(int64_t*)((char*)arr + 0x18);
    if (len <= 0) return NULL;
    return *(void**)((char*)arr + 0x20);
}

static void* xs_find_page(void) {
    if (g_page_inst) return g_page_inst;
    if (!g_core_img || !p_class_get_type || !p_type_get_object) return NULL;
    void* objCls = p_class_from_name(g_core_img, "UnityEngine", "Object");
    if (!objCls) { xs_state("no UnityEngine.Object"); return NULL; }
    void* typeObj = p_type_get_object(p_class_get_type(g_cls_page));
    if (!typeObj) { xs_state("no Type object"); return NULL; }

    void* inst = xs_find_via(objCls, "FindObjectOfType", typeObj);
    if (!inst) inst = xs_find_via(objCls, "FindFirstObjectByType", typeObj);
    if (!inst) {
        void* resCls = p_class_from_name(g_core_img, "UnityEngine", "Resources");
        if (resCls) inst = xs_find_objects_all(resCls, typeObj);
    }
    if (inst) {
        g_page_inst = inst;
        xs_state("running | page ok");
        char b[96]; snprintf(b, sizeof(b), "page inst=%p", inst); xs_crash_note("engine", b);
    } else {
        xs_state("running | no page");
    }
    return inst;
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

        void* page = xs_find_page();
        if (!page) {
            int f = g_fail_ticks.fetch_add(1) + 1;
            {
                char st[96]; snprintf(st, sizeof(st), "running | no page | ticks=%d", f); xs_state(st);
            }
            if (f == 1 || f % 30 == 0) {
                char b[160]; snprintf(b, sizeof(b), "no page (tries=%d, state=%s)", f, g_state);
                xs_crash_note("engine", b);
            }
            return;
        }
        g_fail_ticks.store(0);

        int r1 = -1, r2 = -1;
        if (g_mode_btn.load()) r1 = xs_call0(g_cls_page, page, "OnClickRefresh");
        if (g_mode_search.load()) {
            r2 = xs_call0(g_cls_page, page, "OnClickSearchButton");
            if (r2 != 0) { r2 = xs_call0(g_cls_page, page, "OnClickEnter_Search"); }
        }
        g_calls.fetch_add(1);
        {
            char st[96]; snprintf(st, sizeof(st), "running | page ok | called=%d", g_calls.load()); xs_state(st);
        }
        if (g_calls.load() <= 3) {
            char b[120]; snprintf(b, sizeof(b), "tick#%d refresh=%d search=%d", g_calls.load(), r1, r2);
            xs_crash_note("engine", b);
        }
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
        g_page_inst = NULL;
        uint64_t iv = (uint64_t)g_interval_ms.load() * NSEC_PER_MSEC;
        g_timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_main_queue());
        dispatch_source_set_timer(g_timer, dispatch_time(DISPATCH_TIME_NOW, iv), iv, iv / 10);
        dispatch_source_set_event_handler(g_timer, ^{ xs_tick(); });
        dispatch_resume(g_timer);
        xs_state("running");
        xs_crash_note("engine", "started");
    });
}

extern "C" void xs_engine_stop(void) {
    dispatch_async(dispatch_get_main_queue(), ^{
        g_running.store(false);
        if (g_timer) { dispatch_source_cancel(g_timer); g_timer = nil; }
        xs_state("stopped");
        xs_crash_note("engine", "stopped");
    });
}

extern "C" int xs_engine_calls(void) { return g_calls.load(); }
extern "C" int xs_engine_gap(void) { return g_last_gap.load(); }
