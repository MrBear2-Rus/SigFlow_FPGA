# SigFlow 构建指南（Windows 为主）

> 目标：从 clone 到跑起来。**支持 CMake + MinGW-w64 GCC**（不再支持 Visual Studio/MSVC 构建）。
> 已验证：Windows（MinGW-w64 GCC）+ CMake；Linux（系统 wxGTK）已在真机跑通。
>
> 当前状态：**工具链已插件化**（`InnerPlugin/` 9 个官方插件 + `eda_core`/`eda_platform` 底座）。
> 构建会一并产出核心库与插件；启动时打印 `plugin host diagnostics:`（应见 `[Ready] eda-...`）。

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
| `external.zip` | `external\{fpga-tools,slang-gcc,slang-src,slang-sdk}` | FPGA 工具运行时（`fpga-tools` 必需）；`slang-*` **已不再参与构建**，可忽略 |
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
  -DSIGFLOW_WX_LIB_DIR="$WX/debug/lib"
# 注：不再需要 -DSIGFLOW_SLANG_ROOT（slang 已移除，见 §7）

cmake --build build-gcc --target sigflow --parallel

# 运行（构建后会自动把 wx/MinGW 运行时 DLL 拷到 exe 旁）
.\build-gcc\sigflow.exe
```

- 若 wx 目录结构不同（如 `lib/gcc_x64_dll`），把 `SIGFLOW_WX_CONFIG_DIR` 指到含 `wx/setup.h` 的目录、`SIGFLOW_WX_LIB_DIR` 指到含 `libwx*.a/.dll` 的目录。
- **不再需要 `-DSIGFLOW_SLANG_ROOT`**：slang 死链路已移除，构建不再编译/链接 slang（见 §7）。

### 插件化（P0/P1）后的构建产物

`cmake` 除主程序外，还会构建核心库与官方插件（`InnerPlugin/`）：

| 目标 | 说明 |
| --- | --- |
| `eda_api` | 接口契约（header-only INTERFACE） |
| `eda_core` | 核心内核（PluginHost/JobService/CommandBus/SchemaRegistry/…） |
| `eda_platform` | 平台层（进程/动态库/SHA/平台原语） |
| `eda-synth-yosys` / `eda-pnr-nextpnr` / `eda-pack-gowin` / `eda-program-openfpgaloader` | 工具链插件（综合/布线/打包/烧录） |
| `eda-sim-verilator` / `eda-target-tangnano9k` / `eda-cst-gowin-cst` / `eda-wave-vcd` / `eda-lib-basic` | 仿真/目标板/约束/波形/元件库插件 |
| `sigflow_tree_sitter` | tree-sitter 共享静态库（供 `sigflow` 与 `sig_tree_sync_smoke` 复用） |

> 主程序启动时会打印 `plugin host diagnostics:`；**正常应看到各插件的 `[Ready] eda-...` 行**。
> 若为空，说明静态自注册被链接器丢弃（见 §6 常见问题）。

## 5. 可选：跑单元测试

CMake 构建下已注册以下测试目标（`ctest`）：

| 测试 | 覆盖 |
| --- | --- |
| `eda_api_contract_smoke` | 契约/服务/`IProcessHost` |
| `eda_job_service_smoke` | Job 服务 |
| `eda_toolchain_project_smoke` | 工具发现/工程/内置注册 |
| `eda_plugin_host_smoke` | 插件宿主（真加载 3 个测试插件） |
| `eda_synth_yosys_smoke` / `eda_pnr_nextpnr_smoke` / `eda_pack_flash_smoke` / `eda_sim_verilator_smoke` / `eda_sim_frontend_smoke` | 工具链插件（综合/布线/打包/烧录/仿真） |
| `eda_constraint_sheet_smoke` / `eda_gowin_cst_smoke` | 约束模型/CST 文件校验 |
| `eda_target_profile_smoke` / `eda_component_library_smoke` | 目标板 profile/引脚库/元件库 |
| `eda_wave_vcd_smoke` / `eda_wave_trace_adapter_smoke` | 波形后端/trace 适配 |
| `eda_command_bus_smoke` / `eda_declarative_tool_smoke` / `eda_declarative_switch_smoke` | 命令总线/声明式工具（含"不重编译切换后端"） |
| `sig_tree_sync_smoke` | 双向同步回归（解析→SFTree→`ToVerilog`） |

```powershell
# 配置一个独立的测试构建目录（不会污染 build-gcc）
cmake -S . -B build-contract -G "MinGW Makefiles" `
  -DCMAKE_TOOLCHAIN_FILE="cmake/toolchains/mingw-gcc.cmake" `
  -DMINGW_ROOT="$MINGW" `
  -DSIGFLOW_WX_ROOT="$WX" -DSIGFLOW_WX_CONFIG_DIR="$WX/debug/lib/mswud" -DSIGFLOW_WX_LIB_DIR="$WX/debug/lib"

cmake --build build-contract --parallel
ctest --test-dir build-contract -R "eda_|sig_tree" --output-on-failure
# 期望：100% tests passed（当前 19 项）
```

> 旧的 `job_tests` 仍可单独构建：`cmake --build build-gcc --target job_tests`。

## 6. 常见问题

| 现象 | 原因 / 处理 |
| --- | --- |
| `Cannot find source file: 3rd/json/jsoncpp.cpp` | 没解压 `3rd.zip` → §2 |
| wx 链接大量 undefined / 找不到 `wx/setup.h` | 用了 MSVC 版 wx → 必须用 §3 的 **MinGW 版 wx** |
| `Found package configuration file ... MSVC` 之类 | 同上，wx 目录指错 |
| 编译 `main/FpgaYosysRuntime.h: wx/string.h: No such file` | 目标缺 wx 头目录；已在 CMake 中对 `sigflow_deepseek`/`fpga_flow_probe` 补齐（使用 `SIGFLOW_WX_INCLUDE_DIR`/`SIGFLOW_WX_CONFIG_DIR`） |
| `undefined reference to wxWebRequest...` / `wxEVT_WEBREQUEST_STATE` | DeepSeek 插件需链 **wxNet**（`libwxbase33ud_net.a`）；已在 `Plugin_DeepSeek/CMakeLists.txt` 补齐 |
| 运行时报缺少 `libstdc++-6.dll`/`libgcc_s_seh-1.dll` | 构建时给了 `-DMINGW_ROOT` 会自动拷贝；否则把 MinGW `bin` 加入 PATH |
| CMake 报 “只支持 GCC” | 用 `-G "MinGW Makefiles"` + 上面的 toolchain 文件，别用 VS 生成器 |
| 启动后 `plugin host diagnostics:` 里**没有** `[Ready] eda-...` 行（Help → Toolchain Backends 为空） | 静态自注册被链接器丢弃 / 插件库未链入。已由 `eda_plugin_force_link_*` 锚点 + `link_libraries` 全部官方插件修复；若仍空，检查是否漏链某个 `InnerPlugin/*` |

## 7. 目录与版本约定

- `3rd/`、`external/fpga-tools`、`external/slang-gcc`、`external/slang-src`、`tools/verilator` **不进 git**（体积大），由 zip 提供；**不要把解压产物提交**。
- **slang 已从构建移除**：`UpdateTreeFromSlang` / `AsyncAnalysisCenter::LoadProject` 等死码已删，`CMakeLists.txt` 不再有 `SIGFLOW_SLANG_*`。
  `external/slang-gcc`、`external/slang-sdk`、`external/slang-src` **不再参与构建**（保留目录即可，无需下载；`slang-sdk` 为 MSVC 版，勿提交）。
- 构建产物 `build*/`、日志 `*.log`、`_*.txt`、`tools/_*.sh` 一律不提交。

---

## 附：Linux / macOS 构建（可选）

Linux 构建使用**系统 wxGTK**（不再需要源码 slang）：

```bash
unzip -o 3rd.zip && unzip -o external.zip && unzip -o tools.zip
# 依赖：Arch -> base-devel cmake git python gtk3 wxwidgets-gtk3 mesa glu
#       Ubuntu -> build-essential cmake pkg-config git python3 \
#                 libwxgtk3.0-gtk3-dev libgtk-3-dev libgl1-mesa-dev libglu1-mesa-dev

cmake -S . -B build-linux -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release \
  -DSIGFLOW_USE_LOCAL_WX=OFF
cmake --build build-linux -j$(nproc) 2>&1 | tee build.log
bash tools/smoke_linux.sh build-linux/sigflow 20
```

> - Linux 下 `PlatformProcess` 走 POSIX 后端（`fork/exec` + `setpgid/killpg`），已在真机验证取消超时语义。
> - `-DSIGFLOW_SLANG_*` 已废弃（slang 死码移除，见 §7）。
> - GCC ≥ 14 追加：`-DCMAKE_C_FLAGS="-Wno-error=implicit-function-declaration -Wno-error=implicit-int"`。
> - 可选跑测试：`cmake -S . -B build-contract -DSIGFLOW_USE_LOCAL_WX=OFF && cmake --build build-contract -j && ctest --test-dir build-contract -R "eda_|sig_tree"`。

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

插件源码在 `Plugin_DeepSeek/`。**默认"能装就装"**：Windows 恒装配；Linux 需 libcurl
且 wx 启用了 `wxUSE_WEBREQUEST`（configure 会打印是否装配）。手动开关：

```bash
cmake -S . -B build -DSIGFLOW_BUILD_DEEPSEEK_PLUGIN=ON -DSIGFLOW_EDITION=pro
cmake --build build -j
```

- 产物输出到 `<可执行文件目录>/plugins/DeepSeek_Assistant.(dll|so)`，主程序 `PluginManager`
  自动扫描加载（右侧 "DeepSeek Assistant" 面板）。
- **Windows 预编译 wx 路径**（`SIGFLOW_USE_LOCAL_WX=OFF`）下，插件需额外链接 **wxNet**
  （`libwxbase33ud_net.a`，提供 `wxWebRequest`）；已在 `Plugin_DeepSeek/CMakeLists.txt` 处理。
- 版本标识 `SIGFLOW_EDITION=edu|pro|dev`（编译期宏 `SIGFLOW_EDITION`）；AI 助手为两版共享能力。
- **API Key 不再硬编码**：优先环境变量 `SIGFLOW_DEEPSEEK_API_KEY`，其次用户数据目录下的
  `deepseek.key`（一行）。
- 跨平台 HTTP 走 `wxWebRequest`：Windows 用 WinHTTP（自带）；**Linux 需 libcurl 开发包**，
  且 wx 构建时启用 `wxUSE_WEBREQUEST`（本仓库本地构建 wx 已 `set(wxUSE_WEBREQUEST ON)`）。
  缺失时插件编译会给出明确 `#error`，提示重建 wx。
  - Ubuntu/Debian：`sudo apt install libcurl4-openssl-dev`
  - Arch：`sudo pacman -S curl`
- 已知降级：wxWebRequest 不提供逐块回调，流式输出降级为"完成后一次性回传"（内容一致）。
