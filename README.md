# 一键出 dylib（build-kit）

> 目标产物：**libXSPlugin.dylib**（单文件，注入 cdnf 即用，面板手动开启，不自动扫）
> 这个目录已经是自包含工程：DnfScan.cpp + 桥 + 面板 + CI 全在里面。

---

## 为什么不能直接给你二进制

iOS 的 dylib 必须用 **Apple 工具链 + iPhoneOS SDK** 编译（Mach-O / arm64e / LC_VERSION_MIN），
Windows 上没有 xcrun、没有 iPhoneOS.sdk，**编不出来**。
所以这里给的是「点一下就能出二进制」的工程，三种任选。

---

## 路线 A：GitHub Actions（推荐，不用 Mac，约 5 分钟）

1. 在 GitHub 新建一个**私有仓库**（Private 免费额度足够）；
2. 把 **build-kit 里的所有文件**（含 .github/）传上去：

   ```bash
   cd build-kit
   git init && git add -A && git commit -m "xys scan ios"
   git remote add origin https://github.com/<你的账号>/<仓库>.git
   git push -u origin main
   ```

   > .github 是隐藏目录，网页拖拽上传容易漏；用 git 命令最稳。

3. 打开仓库 → **Actions** → 左侧 build-ios-dylib → 右上 **Run workflow**；
4. 等 3~5 分钟变绿 → 点进那次运行 → 页面底部 **Artifacts** → 下载 libXSPlugin.zip；
5. 解压得到 libXSPlugin.dylib。

失败就看日志里 make 那一步的报错，贴给我。

---

## 路线 B：自己的 Mac / 朋友的 Mac

```bash
cd build-kit
bash build.sh
# 产出 ios-src/libXSPlugin.dylib
```

要求：macOS + Xcode Command Line Tools（`xcode-select --install`）。

---

## 路线 C：云 Mac

MacinCloud / Scaleway Mac mini 之类，按小时租一台，登录后 `bash build.sh` 即可。

---

## 拿到 dylib 之后

```text
1. 注入目标：com.tencent.cdnf
2. 工具：TrollFools / Sideloadly / 越狱 dylib 注入（跟你注入 H5GG 的方式一样）
3. 进游戏 → 1.5 秒后面板自动弹出（440×620，位置 34,56）
4. 面板底部显示「内核 已连接 [native]」= 通了
5. 手动勾「扫拍刷新」才开始，默认不扫
```

> 顶部标题栏可拖动；底部「隐藏」收成小圆球。
> 想改 UI：把新的 index.html 丢到 /var/mobile/Documents/xys/index.html，重启游戏即生效，不用重编。

---

## 工程结构

```text
build-kit/
├─ build.sh                      一条命令：打补丁 + 内嵌 HTML + 编译
├─ .github/workflows/build.yml   GitHub Actions（macOS runner + artifact）
├─ DnfScan.cpp                   安卓核心（原样，构建时自动改造）
├─ tools/patch_for_ios.py        自动改造脚本（已在本机验证跑通）
├─ ios-src/
│   ├─ XSPlugin.mm               JSON 入口 xs_api_call()
│   ├─ XSPanel.mm                UIKit 悬浮窗 + WKWebView + 内嵌面板
│   ├─ XSAPI.h                   内核函数声明
│   ├─ platform_ios.h            dyld / Mach-O / mono_ms（替代安卓 ELF 那套）
│   ├─ xs_hook.h                 指针替换（不碰 .text）
│   ├─ include/android/log.h     兼容垫片
│   └─ Makefile                  xcrun 编译规则（自动生成 XSHTML.h）
└─ script/index.html             面板（编译进 dylib）
```

## patch_for_ios.py 到底改了什么

| # | 改动 | 原因 |
|---|---|---|
| 1 | include android/log.h → platform_ios.h | iOS 没有 log.h |
| 2 | include elf.h 整行 #if 0 | macOS 无 elf.h |
| 3 | maps_first_base() 安卓实现整段 #if 0 | 改用 dyld 镜像表 |
| 4 | elf_find_sym() 安卓实现整段 #if 0 | 改用 dlsym + Mach-O 符号表 |
| 5 | "libil2cpp.so" → "UnityFramework" | iOS 把 il2cpp 编进 UnityFramework |
| 6 | 私有路径 → /var/mobile/Documents/xys/ | iOS 沙盒 |

业务逻辑一行没动（四方式、购买 A/B/C、限购、统计全保留）。

---

## 常见编译报错

| 报错 | 原因 | 处理 |
|---|---|---|
| json.hpp not found | 没带 nlohmann | 把安卓工程里的 App/json.hpp 放到 ios-src/../DNF/ 或改 Makefile 的 -I |
| Use of undeclared identifier p_class_from_name | 补丁没跑 | 确认 build.sh 里 patch 步骤成功；ios-src/DnfScan.cpp 里搜 UnityFramework |
| DobbyHook not found | USE_DOBBY 打开了 | 默认 USE_DOBBY=0，不需要 Dobby |
