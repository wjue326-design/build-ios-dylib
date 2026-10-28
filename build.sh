#!/usr/bin/env bash
# build.sh —— 一条命令产出 libXSPlugin.dylib（需要在 macOS 上跑）
set -euo pipefail
cd "$(dirname "$0")"
ROOT="$(pwd)"

if ! command -v xcrun >/dev/null 2>&1; then
  echo "[ERR] 没有 xcrun：必须在 macOS（Xcode Command Line Tools）上跑"
  echo "      Windows 请用 GitHub Actions（.github/workflows/build.yml）"
  exit 1
fi

echo "== 1/4 改造 DnfScan.cpp =="
python3 tools/patch_for_ios.py DnfScan.cpp ios-src/DnfScan.cpp

echo "== 2/4 Dobby（按架构各编一份） =="
mkdir -p ios-src/libs
if [ ! -d third_party/Dobby ]; then
  git clone --depth 1 https://github.com/jmpews/Dobby.git third_party/Dobby
fi
for ARCH in arm64 arm64e; do
  if [ -f "ios-src/libs/libdobby.$ARCH.a" ]; then echo "   $ARCH 已有，跳过"; continue; fi
  rm -rf "third_party/Dobby/build-$ARCH"
  cd third_party/Dobby
  mkdir -p "build-$ARCH" && cd "build-$ARCH"
  cmake .. -DCMAKE_SYSTEM_NAME=iOS \
           -DCMAKE_SYSTEM_PROCESSOR=$ARCH \
           -DCMAKE_OSX_ARCHITECTURES=$ARCH \
           -DCMAKE_OSX_DEPLOYMENT_TARGET=14.0 \
           -DCMAKE_BUILD_TYPE=Release
  make -j4
  LIB=$(find . -name 'libdobby*.a' | head -1)
  cp "$LIB" "$ROOT/ios-src/libs/libdobby.$ARCH.a"
  cd "$ROOT"
  echo "   $ARCH ok"
done

echo "== 3/4 生成内嵌 HTML =="
cd "$ROOT/ios-src"
if command -v xxd >/dev/null 2>&1; then
  xxd -i -n XSHTML_DATA ../script/index.html > XSHTML.h
else
  python3 -c "d=open('../script/index.html','rb').read();print('unsigned char XSHTML_DATA[] = {'+','.join(str(b) for b in d)+'};');print('unsigned int XSHTML_DATA_len = %d;'%len(d))" > XSHTML.h
fi

echo "== 4/4 编译 dylib（双架构 + lipo） =="
cd "$ROOT/ios-src"
make clean >/dev/null 2>&1 || true
for ARCH in arm64 arm64e; do
  make ARCH=$ARCH OUT=libXSPlugin.$ARCH.dylib
done
lipo -create -output libXSPlugin.dylib libXSPlugin.arm64.dylib libXSPlugin.arm64e.dylib
echo "   lipo ok"

# ad-hoc 签名：TrollFools / 部分注入器要求 dylib 带签名
codesign -f -s - libXSPlugin.dylib 2>/dev/null || echo "   (codesign skipped)"
codesign -dv libXSPlugin.dylib 2>&1 | head -3 || true

echo
echo "OK -> $ROOT/ios-src/libXSPlugin.dylib"
echo "注入 cdnf 后进游戏，1.5 秒弹面板；扫拍要手动勾。"
