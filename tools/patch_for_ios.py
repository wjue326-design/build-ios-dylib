#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""把 Android 版 DnfScan.cpp 机械改造成 iOS 可编译版本。

改动（都是 platform_ios.h 接管的部分，不改业务逻辑）：
  1. #include <android/log.h>   -> #include "platform_ios.h"
  2. #include <elf.h>           -> 整行禁用（macOS 无 elf.h）
  3. maps_first_base()          -> 安卓实现整段 #if 0（iOS 用 dyld 版）
  4. elf_find_sym()             -> 安卓实现整段 #if 0（iOS 用 Mach-O 版）
  5. "libil2cpp.so"             -> "UnityFramework"
  6. /data/data/com.tencent.tmgp.dnf/files/ -> /var/mobile/Documents/xys/
  7. /sdcard/DCIM/Screenshots/  -> /var/mobile/Media/DCIM/

用法: python3 patch_for_ios.py DnfScan.cpp out/DnfScan.cpp
"""
import sys, os

def skip_literal(s, i):
    """返回跳过字符串/字符/注释后的下标（i 处不是字面量则原样返回）"""
    if s.startswith('//', i):
        j = s.find('\n', i)
        return len(s) if j < 0 else j
    if s.startswith('/*', i):
        j = s.find('*/', i + 2)
        return len(s) if j < 0 else j + 2
    if s[i] in '"\'':
        q = s[i]
        j = i + 1
        while j < len(s):
            if s[j] == '\\':
                j += 2
                continue
            if s[j] == q:
                return j + 1
            j += 1
        return len(s)
    return i

def find_block(text, signature):
    i = text.find(signature)
    if i < 0:
        return None
    j = text.find('{', i)
    depth = 0
    while j < len(text):
        nj = skip_literal(text, j)
        if nj != j:
            j = nj
            continue
        c = text[j]
        if c == '{':
            depth += 1
        elif c == '}':
            depth -= 1
            if depth == 0:
                return (i, j + 1)
        j += 1
    return None

def wrap_if0(text, signature, tag):
    blk = find_block(text, signature)
    if not blk:
        print('  [skip] 未找到 ' + tag)
        return text
    a, b = blk
    return text[:a] + '#if 0 /* iOS: ' + tag + ' 由 platform_ios.h 提供 */\n' + text[a:b] + '\n#endif\n' + text[b:]

def main():
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(1)
    src, dst = sys.argv[1], sys.argv[2]
    t = open(src, 'r', encoding='utf-8', errors='replace').read()
    orig_len = len(t)

    t = t.replace('#include <android/log.h>', '#include "platform_ios.h"')
    t = t.replace('#include <elf.h>', '#if 0 /* iOS: 无 elf.h */\n#include <elf.h>\n#endif')

    t = wrap_if0(t, 'static void* maps_first_base(', 'maps_first_base')
    t = wrap_if0(t, 'static void* elf_find_sym(', 'elf_find_sym')

    t = t.replace('"libil2cpp.so"', '"UnityFramework"')
    t = t.replace('/data/data/com.tencent.tmgp.dnf/files/', '/var/mobile/Documents/xys/')
    t = t.replace('/sdcard/DCIM/Screenshots/', '/var/mobile/Media/DCIM/')

    os.makedirs(os.path.dirname(dst) or '.', exist_ok=True)
    open(dst, 'w', encoding='utf-8').write(t)
    print('patched: %s -> %s  (%d -> %d bytes)' % (src, dst, orig_len, len(t)))

if __name__ == '__main__':
    main()
