// xs_hooklog.h —— 给每个 DobbyHook 调用打点（崩了能看出是哪一个）
#pragma once
#include <cstdio>
#include <dlfcn.h>

extern "C" void xs_crash_note(const char* tag, const char* detail);

/* 通过 asm 标签直接引用真符号（DobbyHook 是 hidden 的，dlsym 找不到） */
extern "C" int xs_DobbyHook_real(void* addr, void* repl, void** orig) __asm__("_DobbyHook");

static inline int xs_hook_logged(void* addr, void* repl, void** orig, int line){
    char b[192];
    snprintf(b, sizeof(b), "hook L%d addr=%p repl=%p", line, addr, repl);
    xs_crash_note("hook", b);
    int r = xs_DobbyHook_real(addr, repl, orig);
    snprintf(b, sizeof(b), "hook L%d ret=%d orig=%p", line, r, (orig && *orig) ? *orig : 0);
    xs_crash_note("hook", b);
    return r;
}
#define DobbyHook(a, b, c) xs_hook_logged((void*)(a), (void*)(b), (void**)(c), __LINE__)
