// XSCrash.mm —— 装进 dylib，闪退时把原因写到文件里
#import <Foundation/Foundation.h>
#include <execinfo.h>
#include <signal.h>
#include <unistd.h>
#include <string.h>

static NSString* xs_log_path(void){
    NSString* dir = [NSHomeDirectory() stringByAppendingPathComponent:@"Documents/xys"];
    [[NSFileManager defaultManager] createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:nil];
    return [dir stringByAppendingPathComponent:@"crash.log"];
}

static void xs_write(const char* tag, const char* detail){
    @autoreleasepool {
        NSString* p = xs_log_path();
        NSMutableString* s = [NSMutableString string];
        [s appendFormat:@"==== %@  %s\n", [NSDate date], tag];
        if (detail) [s appendFormat:@"%s\n", detail];
        void* bt[64];
        int n = backtrace(bt, 64);
        char** syms = backtrace_symbols(bt, n);
        for (int i = 0; i < n && syms; i++) [s appendFormat:@"%s\n", syms[i]];
        if (syms) free(syms);
        NSFileHandle* fh = [NSFileHandle fileHandleForWritingAtPath:p];
        if (!fh) { [s writeToFile:p atomically:YES encoding:NSUTF8StringEncoding error:nil]; }
        else { [fh seekToEndOfFile]; [fh writeData:[s dataUsingEncoding:NSUTF8StringEncoding]]; [fh closeFile]; }
    }
}

static void xs_signal_handler(int sig){
    xs_write("signal", strsignal(sig));
    signal(sig, SIG_DFL);
    raise(sig);
}

static void xs_exception_handler(NSException* e){
    NSString* d = [NSString stringWithFormat:@"%@\n%@", e.reason ?: @"", e.callStackSymbols ?: @[]];
    xs_write("exception", d.UTF8String);
}

extern "C" void xs_crash_note(const char* tag, const char* detail){ xs_write(tag, detail); }

__attribute__((constructor)) static void xs_crash_install(void){
    NSSetUncaughtExceptionHandler(&xs_exception_handler);
    signal(SIGSEGV, xs_signal_handler);
    signal(SIGBUS,  xs_signal_handler);
    signal(SIGABRT, xs_signal_handler);
    signal(SIGILL,  xs_signal_handler);
    signal(SIGFPE,  xs_signal_handler);
    signal(SIGTRAP, xs_signal_handler);
    xs_write("boot", "dylib loaded");
}
