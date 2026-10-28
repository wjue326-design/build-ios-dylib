// xs_hook.h —— 替代 DobbyHook 的「方法指针替换」层
//
// 为什么要有这个：
//   dnfm-dump 的检测链台账里，M1(代码段 CRC) / M6(函数头 16 字节扫描) 两条
//   专门抓 inline hook。DobbyHook 一定改函数头 → 必被抓。
//   而 il2cpp 的 MethodInfo +0x00 是 methodPointer（数据），改它不动 .text。
//
// 用法（把 DnfScan.cpp 里的 DobbyHook 直接替换即可）：
//   DobbyHook(*(void**)m, (void*)my_cb, (void**)&orig_cb);
// → XS_HOOK_METHOD(m, my_cb, orig_cb);
//
#pragma once
#include <stdint.h>
#include <string.h>
#include <mach-o/dyld.h>

#if __has_include(<ptrauth.h>)
#include <ptrauth.h>
#define XS_HAS_PTRAUTH 1
#else
#define XS_HAS_PTRAUTH 0
#endif

#ifndef XS_USE_DOBBY
#define XS_USE_DOBBY 0          /* 默认不链 Dobby */
#endif

/* 每个被替换的方法最多记住 128 个，够用且不占堆 */
#define XS_MAX_HOOKS 128
typedef struct { void* method; void* orig; void* repl; } xs_hook_slot;
static xs_hook_slot g_xs_hooks[XS_MAX_HOOKS];
static int g_xs_hook_n = 0;

static inline void* xs_strip(void* p){
#if XS_HAS_PTRAUTH
    return ptrauth_strip(p, ptrauth_key_asia);
#else
    return p;
#endif
}
static inline void* xs_sign(void* p){
#if XS_HAS_PTRAUTH
    return ptrauth_sign_unauthenticated(p, ptrauth_key_asia, 0);
#else
    return p;
#endif
}

/* 读 MethodInfo 里的原始函数指针 */
static inline void* xs_method_pointer(void* methodInfo){
    if (!methodInfo) return 0;
    return xs_strip(*(void**)methodInfo);
}

/* 替换：只写 MethodInfo+0x00（数据段），不碰代码 */
static inline int xs_hook_method(void* methodInfo, void* replace, void** origin){
    if (!methodInfo || !replace) return -1;
    void* orig = xs_method_pointer(methodInfo);
    if (orig == replace) return 0;                 /* 已经换过 */
    if (g_xs_hook_n < XS_MAX_HOOKS){
        g_xs_hooks[g_xs_hook_n].method = methodInfo;
        g_xs_hooks[g_xs_hook_n].orig   = orig;
        g_xs_hooks[g_xs_hook_n].repl   = replace;
        g_xs_hook_n++;
    }
    if (origin) *origin = orig;
    *(void**)methodInfo = xs_sign(replace);
    return 0;
}

/* 还原全部（退出/关闭功能时调用） */
static inline void xs_unhook_all(void){
    for (int i = 0; i < g_xs_hook_n; i++){
        void* m = g_xs_hooks[i].method;
        if (!m) continue;
        *(void**)m = xs_sign(g_xs_hooks[i].orig);
    }
    g_xs_hook_n = 0;
}

/* 手动调一次游戏方法（主动调用路径用，不需要 hook） */
static inline void xs_call0(void* methodInfo, void* self){
    void* fn = xs_method_pointer(methodInfo);
    if (fn) ((void(*)(void*))fn)(self);
}
static inline void xs_call1(void* methodInfo, void* self, void* a){
    void* fn = xs_method_pointer(methodInfo);
    if (fn) ((void(*)(void*, void*))fn)(self, a);
}
static inline void xs_call2(void* methodInfo, void* self, void* a, void* b){
    void* fn = xs_method_pointer(methodInfo);
    if (fn) ((void(*)(void*, void*, void*))fn)(self, a, b);
}
static inline void xs_call3(void* methodInfo, void* self, void* a, void* b, void* c){
    void* fn = xs_method_pointer(methodInfo);
    if (fn) ((void(*)(void*, void*, void*, void*))fn)(self, a, b, c);
}

/* ---------- 兼容层：源码里 DobbyHook 一字不改也能编 ---------- */
#if XS_USE_DOBBY
#include <dobby.h>
#define XS_HOOK_METHOD(methodInfo, replace, origin) DobbyHook(*(void**)(methodInfo), (void*)(replace), (void**)(origin))
#else
#define XS_HOOK_METHOD(methodInfo, replace, origin) xs_hook_method((void*)(methodInfo), (void*)(replace), (void**)(origin))
#endif
