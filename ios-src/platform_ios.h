// platform_ios.h — Android → iOS 平台层替身
// 目标: 让 DnfScan.cpp 主体不改一行, 也能在 iOS 上编译
//
// 提供:
//   LOGI/LOGE/LOGD   ← 替代 android/log.h
//   maps_first_base  ← 替代 /proc/self/maps 扫描(安卓), 改为 dyld 镜像表
//   elf_find_sym     ← 替代 ELF 符号表扫描, 改为 dlsym + Mach-O LC_SYMTAB
//   mono_ms()        ← 单调时钟
#pragma once
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <unistd.h>
#include <dlfcn.h>
#include <time.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <mach-o/nlist.h>
#import <Foundation/Foundation.h>

#define LOG_TAG "DNF_HOOK"
#define LOGI(fmt, ...)  NSLog((@"[DNF] "   fmt), ##__VA_ARGS__)
#define LOGE(fmt, ...)  NSLog((@"[DNF][E] " fmt), ##__VA_ARGS__)
#define LOGD(fmt, ...)  NSLog((@"[DNF][D] " fmt), ##__VA_ARGS__)

static inline long long mono_ms(void){
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

// 取基线: 返回首个 basename == name 的镜像头地址(即 __TEXT 加载地址)
static inline void* maps_first_base(const char* name){
    uint32_t n = _dyld_image_count();
    for (uint32_t i = 0; i < n; i++){
        const char* path = _dyld_get_image_name(i);
        if (!path) continue;
        const char* slash = strrchr(path, '/');
        const char* base = slash ? slash + 1 : path;
        if (strcmp(base, name) == 0) return (void*)_dyld_get_image_header(i);
    }
    // 兼容: 传 "UnityFramework" 但实际是 UnityFramework.framework/UnityFramework
    for (uint32_t i = 0; i < n; i++){
        const char* path = _dyld_get_image_name(i);
        if (path && strstr(path, name)) return (void*)_dyld_get_image_header(i);
    }
    return NULL;
}

// Mach-O 符号查找(替代安卓 ELF .dynsym 扫描)
static inline void* elf_find_sym(void* hinge, const char* name){
    if (!hinge || !name) return NULL;

    // 1) 先试 dlsym: iOS 上 UnityFramework 大部分 il2cpp_* 是导出符号
    void* p = dlsym(RTLD_DEFAULT, name);
    if (p) return p;
    std::string us = std::string("_") + name;
    p = dlsym(RTLD_DEFAULT, us.c_str());
    if (p) return p;

    // 2) 扫 LC_SYMTAB
    const struct mach_header_64* hdr = (const struct mach_header_64*)hinge;
    const uintptr_t hingeAddr = (uintptr_t)hinge;
    if (hdr->magic != MH_MAGIC_64) return NULL;

    const struct load_command* lc = (const struct load_command*)((const char*)hdr + sizeof(struct mach_header_64));
    const struct symtab_command* symtab = NULL;
    uintptr_t slide = 0;
    for (uint32_t i = 0; i < hdr->ncmds; i++){
        if (lc->cmd == LC_SYMTAB) symtab = (const struct symtab_command*)lc;
        else if (lc->cmd == LC_SEGMENT_64){
            const struct segment_command_64* seg = (const struct segment_command_64*)lc;
            if (strcmp(seg->segname, "__TEXT") == 0) slide = hingeAddr - (uintptr_t)seg->vmaddr;
        }
        lc = (const struct load_command*)((const char*)lc + lc->cmdsize);
    }
    if (!symtab) return NULL;

    const struct nlist_64* syms = (const struct nlist_64*)((const char*)hdr + symtab->symoff);
    const char* strs = (const char*)hdr + symtab->stroff;
    for (uint32_t i = 0; i < symtab->nsyms; i++){
        if (syms[i].n_un.n_strx == 0) continue;
        const char* sn = strs + syms[i].n_un.n_strx;
        if (strcmp(sn, us.c_str()) == 0)
            return (void*)(syms[i].n_value + slide);
    }
    return NULL;
}
