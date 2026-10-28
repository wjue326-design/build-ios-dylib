// include/android/log.h — 兼容垫片
// DnfScan.cpp 里 #include <android/log.h> 保持不动, 编译时 -Iinclude 就能命中这里
#pragma once
#import "platform_ios.h"
#define ANDROID_LOG_INFO  4
#define ANDROID_LOG_ERROR 6
#define ANDROID_LOG_DEBUG 3
static inline int __android_log_print(int, const char* tag, const char* fmt, ...) { (void)tag; (void)fmt; return 0; }
