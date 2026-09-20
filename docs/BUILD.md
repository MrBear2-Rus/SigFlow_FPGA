# SigFlow 构建指南（Windows 为主）

> 目标：从 clone 到跑起来。**支持 CMake + MinGW-w64 GCC**（不再支持 Visual Studio/MSVC 构建）。
> 已验证：Windows（MinGW-w64 GCC）+ CMake。

---

## 0. 三步概览

```powershell
git clone <仓库地址>
cd <仓库目录>
# 解压依赖包（见 §2）
# 配置 + 构建（见 §4）
```

## 1. 准备工具

| 工具 | 说明 |
| --- | --- |
| **MinGW-w64 GCC**（x86_64） | 必须；含 `gcc/g++/make/windres`。建议 12+ |
| **CMake** ≥ 3.21 | 生成器用 `MinGW Makefiles` |
| 解压工具 | 用于解压 zip（7-Zip / PowerShell 皆可） |

> ⚠️ **不要用 Visual Studio 生成器**：本仓库只支持 CMake + GCC。

## 2. 解压依赖包（必须）

仓库用 git 跟踪了几个压缩包，但**解压出来的目录被 .gitignore 排除**，clone 后必须解压到**仓库根目录**：

```powershell
# PowerShell 解压（或用 7-Zip 右键解压到当前目录）
Expand-Archive -Force 3rd.zip .
Expand-Archive -Force external.zip .
Expand-Archive -Force tools.zip .
# MinGW 版 wxWidgets（项目负责人分发），解压到 3rd\ 下 → 3rd\wx-mingw\
Expand-Archive -Force wx-mingw.zip 3rd\
```

| 压缩包 | 解压出 | 用途 |
| --- | --- | --- |
| `3rd.zip` | `3rd\{json,nlohmann,nanosvg,tree-sitter,tree-sitter-verilog,vcd,wxWidgets-3.2.9}` | 主程序第三方源码（`wxWidgets-3.2.9` 是 **MSVC 版**，GCC 构建不使用） |
| `external.zip` | `external\{fpga-tools,slang-gcc,slang-src,slang-sdk}` | FPGA 工具运行时；**`slang-gcc` 是 MinGW GCC 版 slang**（Windows 构建用它） |
| `tools.zip` | `tools\verilator` + 脚本 | 仿真用 Verilator（Windows） |
| `wx-mingw.zip`（项目负责人分发） | `3rd\wx-mingw\{include,debug,bin,...}` | **MinGW 版 wxWidgets**（Windows 构建必须，见 §3） |

## 3. MinGW 版 wxWidgets（Windows 构建必需）

`3rd/wxWidgets-3.2.9` 只有 MSVC 库，**MinGW 链接不了**。请使用项目负责人分发的 `wx-mingw.zip`，解压到 **`3rd\wx-mingw\`**（`3rd/` 已被 `.gitignore` 忽略，无需改 ignore；内含 `include\`、`debug\lib\`、`bin\`）。

> 若你手上还没有，向项目负责人索取 `wx-mingw.zip`（即 vcpkg 的 `x64-mingw-dynamic` 安装目录）。

## 4. 配置 + 构建 + 运行

```powershell
# 变量：按你的实际路径修改
$MINGW = "E:/path/to/mingw64"                       # MinGW-w64 根目录（含 bin/g++.exe）
$WX    = "E:/path/to/repo/3rd/wx-mingw"            # MinGW 版 wx 根目录（含 include/）

cmake -S . -B build-gcc -G "MinGW Makefiles" `
  -DCMAKE_TOOLCHAIN_FILE="cmake/toolchains/mingw-gcc.cmake" `
  -DMINGW_ROOT="$MINGW" `
  -DSIGFLOW_WX_ROOT="$WX" `
  -DSIGFLOW_WX_CONFIG_DIR="$WX/debug/lib/mswud" `
  -DSIGFLOW_WX_LIB_DIR="$WX/debug/lib" `
  -DSIGFLOW_SLANG_ROOT="E:/path/to/repo/external/slang-gcc"

cmake --build build-gcc --target sigflow --parallel

# 运行（构建后会自动把 wx/MinGW 运行时 DLL 拷到 exe 旁）
.\build-gcc\sigflow.exe
```

- 若 wx 目录结构不同（如 `lib/gcc_x64_dll`），把 `SIGFLOW_WX_CONFIG_DIR` 指到含 `wx/setup.h` 的目录、`SIGFLOW_WX_LIB_DIR` 指到含 `libwx*.a/.dll` 的目录。
- `-DSIGFLOW_SLANG_ROOT` 指向 `external/slang-gcc`（MinGW GCC 版 slang）。

## 5. 可选：跑单元测试

```powershell
cmake --build build-gcc --target job_tests --parallel
.\build-gcc\tests\job_tests.exe
# 期望： ALL TESTS PASSED
```

## 6. 常见问题

| 现象 | 原因 / 处理 |
| --- | --- |
| `Cannot find source file: 3rd/json/jsoncpp.cpp` | 没解压 `3rd.zip` → §2 |
| wx 链接大量 undefined / 找不到 `wx/setup.h` | 用了 MSVC 版 wx → 必须用 §3 的 **MinGW 版 wx** |
| `Found package configuration file ... MSVC` 之类 | 同上，wx 目录指错 |
| `Could not find ... svlang` | `-DSIGFLOW_SLANG_ROOT` 没指到 `external/slang-gcc` |
| 运行时报缺少 `libstdc++-6.dll`/`libgcc_s_seh-1.dll` | 构建时给了 `-DMINGW_ROOT` 会自动拷贝；否则把 MinGW `bin` 加入 PATH |
| CMake 报 “只支持 GCC” | 用 `-G "MinGW Makefiles"` + 上面的 toolchain 文件，别用 VS 生成器 |

## 7. 目录与版本约定

- `3rd/`、`external/fpga-tools`、`external/slang-gcc`、`external/slang-src`、`tools/verilator` **不进 git**（体积大），由 zip 提供；**不要把解压产物提交**。
- `external/slang-sdk` 是 MSVC 版、Windows 构建用不到，请勿提交。
- 构建产物 `build*/`、日志 `*.log`、`_*.txt`、`tools/_*.sh` 一律不提交。

---

## 附：Linux / macOS 构建（可选）

Linux 构建使用**系统 wxGTK + 源码 slang**（`external/slang-sdk` 在 Linux 上不可用）：

```bash
unzip -o 3rd.zip && unzip -o external.zip && unzip -o tools.zip
# 依赖：Arch -> base-devel cmake git python gtk3 wxwidgets-gtk3 mesa glu
#       Ubuntu -> build-essential cmake pkg-config git python3 \
#                 libwxgtk3.0-gtk3-dev libgtk-3-dev libgl1-mesa-dev libglu1-mesa-dev
# slang 依赖（fmt/boost::regex/tomlplusplus）：
mkdir -p ~/slang-deps && cd ~/slang-deps
git clone --depth 1 --branch 12.2.0 https://gitee.com/mirrors/fmt.git fmt
git clone --depth 1 --branch boost-1.91.0 https://gitclone.com/github.com/MikePopoloski/regex.git regex
git clone --depth 1 https://gitclone.com/github.com/marzer/tomlplusplus.git tomlplusplus
cd -

cmake -S . -B build-linux -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release \
  -DSIGFLOW_USE_LOCAL_WX=OFF -DSIGFLOW_BUILD_SLANG_FROM_SOURCE=ON -DSIGFLOW_SLANG_FETCH_DEPS=ON \
  -DFETCHCONTENT_SOURCE_DIR_FMT=$HOME/slang-deps/fmt \
  -DFETCHCONTENT_SOURCE_DIR_BOOST_REGEX=$HOME/slang-deps/regex \
  -DFETCHCONTENT_SOURCE_DIR_TOMLPLUSPLUS=$HOME/slang-deps/tomlplusplus
cmake --build build-linux -j$(nproc) 2>&1 | tee build.log
bash tools/smoke_linux.sh build-linux/sigflow 20
```
> GCC ≥ 14 追加：`-DCMAKE_C_FLAGS="-Wno-error=implicit-function-declaration -Wno-error=implicit-int"`。

---

## 附录 A：维护者如何生成 `wx-mingw.zip`（供分发）

MinGW 版 wxWidgets 常来自 vcpkg 的 `x64-mingw-dynamic` 安装目录（含 `include/`、`debug/lib/`、`bin/`）。只需打包这三部分：

```powershell
$wx    = "E:/download/vcpkg-master/installed/x64-mingw-dynamic"
$stage = "$env:TEMP/wx-mingw"
Remove-Item -Recurse -Force $stage -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $stage | Out-Null
Copy-Item "$wx/include" "$stage/include" -Recurse
Copy-Item "$wx/debug"   "$stage/debug"   -Recurse
Copy-Item "$wx/bin"     "$stage/bin"     -Recurse
Compress-Archive -Path "$stage" -DestinationPath wx-mingw.zip
```

生成后分发给同伴。同伴解压到仓库的 **`3rd\`** 下即可得到 `3rd\wx-mingw\`（`3rd/` 已被 `.gitignore` 忽略，不会进 git，无需改动 `.gitignore`）。


---

## 附录 B：DeepSeek AI 助手插件（跨平台，可选）

插件源码在 `Plugin_DeepSeek/`，默认**不构建**。开启：

```bash
cmake -S . -B build -DSIGFLOW_BUILD_DEEPSEEK_PLUGIN=ON -DSIGFLOW_EDITION=pro
cmake --build build -j
```

- 产物输出到 `<可执行文件目录>/plugins/DeepSeek_Assistant.(dll|so)`，主程序 `PluginManager`
  自动扫描加载（右侧 "DeepSeek Assistant" 面板）。
- 版本标识 `SIGFLOW_EDITION=edu|pro|dev`（编译期宏 `SIGFLOW_EDITION`）；AI 助手为两版共享能力。
- **API Key 不再硬编码**：优先环境变量 `SIGFLOW_DEEPSEEK_API_KEY`，其次用户数据目录下的
  `deepseek.key`（一行）。
- 跨平台 HTTP 走 `wxWebRequest`：Windows 用 WinHTTP（自带）；**Linux 需 libcurl 开发包**，
  且 wx 构建时启用 `wxUSE_WEBREQUEST`（本仓库本地构建 wx 已 `set(wxUSE_WEBREQUEST ON)`）。
  缺失时插件编译会给出明确 `#error`，提示重建 wx。
  - Ubuntu/Debian：`sudo apt install libcurl4-openssl-dev`
  - Arch：`sudo pacman -S curl`
- 已知降级：wxWebRequest 不提供逐块回调，流式输出降级为"完成后一次性回传"（内容一致）。
