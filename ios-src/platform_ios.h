// platform_ios.h — Android → iOS 平台层替身（纯 C，禁止出现 ObjC 语法）
//
// DnfScan.cpp 是 .cpp（C++），不能 include Foundation，所以这里只用 C：
//   LOGI/LOGE/LOGD   ← 替代 android/log.h（直接写 stderr）
//   maps_first_base  ← 替代 /proc/self/maps（改 dyld 镜像表）
//   elf_find_sym     ← 替代 ELF 符号表（改 dlsym + Mach-O LC_SYMTAB）
//   mono_ms()        ← 单调时钟
#pragma once
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <unistd.h>
#include <dlfcn.h>
#include <time.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <mach-o/nlist.h>

#define LOG_TAG "DNF_HOOK"
#define XS_LOG(level, fmt, ...) fprintf(stderr, "[DNF][%s] " fmt "\n", level, ##__VA_ARGS__)
#define LOGI(fmt, ...) XS_LOG("I", fmt, ##__VA_ARGS__)
#define LOGE(fmt, ...) XS_LOG("E", fmt, ##__VA_ARGS__)
#define LOGD(fmt, ...) XS_LOG("D", fmt, ##__VA_ARGS__)

/* mono_ms: DnfScan.cpp 自己有一份，这里不重复定义 */

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
    // 兼容: 传 "UnityFramework" 但镜像是 .../UnityFramework.framework/UnityFramework
    for (uint32_t i = 0; i < n; i++){
        const char* path = _dyld_get_image_name(i);
        if (path && strstr(path, name)) return (void*)_dyld_get_image_header(i);
    }
    return NULL;
}

// Mach-O 符号查找（UnityFramework 已导出 241 个 _il2cpp_*）
static inline void* elf_find_sym(void* hinge, const char* name){
    if (!hinge || !name) return NULL;

    void* p = dlsym(RTLD_DEFAULT, name);
    if (p) return p;
    char us[256];
    snprintf(us, sizeof(us), "_%s", name);
    p = dlsym(RTLD_DEFAULT, us);
    if (p) return p;

    const struct mach_header_64* hdr = (const struct mach_header_64*)hinge;
    if (hdr->magic != MH_MAGIC_64) return NULL;
    const uintptr_t hingeAddr = (uintptr_t)hinge;

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
        if (strcmp(sn, us) == 0) return (void*)(syms[i].n_value + slide);
    }
    return NULL;
}
