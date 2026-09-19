# FPGA 工具链的跨平台源码构建（yosys / nextpnr / apicula）

## 背景

仓库里 `external/fpga-tools/runtime/` 随包分发的工具**全是 Windows PE 二进制**
（`yosys.exe`、`nextpnr-himbaechel.exe`、`openFPGALoader.exe`、apicula 的 Windows venv），
Linux 上一个都不能执行；而 `FindFpgaTool()` 在 Linux 上按平台规则查找的是**无扩展名**的
`yosys` / `nextpnr-himbaechel`，因此必然找不到。

本方案让 **Windows 与 Linux 用同一套源码、各自本地编译**，并由 CMake 统一管理。

## 快速开始

```bash
# 1) 打开开关（默认关闭，不影响普通构建）
cmake -S . -B build -DSIGFLOW_FETCH_FPGA_TOOLS=ON

# 2) 单独构建工具链（这一步很慢：yosys 约 5–15 分钟，nextpnr 约 10–30 分钟）
cmake --build build --target sigflow_fpga_tools -j

# 3) 之后正常构建主程序即可，它会自动在 external/fpga-tools/runtime/ 找到工具
cmake --build build -j
```

国内/内网环境（GitHub 不可达时）：

```bash
cmake -S . -B build -DSIGFLOW_FETCH_FPGA_TOOLS=ON \
      -DSIGFLOW_FPGA_TOOLS_URL_PREFIX=https://gitclone.com/github.com/
```

## 工作方式

```
FetchContent  ──下载同一套源码──▶  _deps/<tool>-src
                                       │
                                       ├─ yosys    : make config-gcc → make → make install
                                       ├─ nextpnr  : cmake → cmake --build → cmake --install
                                       └─ apicula  : python -m venv → pip install .
                                       ▼
                       "落位"到主程序既有的查找布局：
       external/fpga-tools/runtime/yosys/{bin,share}/…
       external/fpga-tools/runtime/nextpnr/{bin,share}/…
       external/fpga-tools/runtime/apicula/{bin,Scripts,Lib}/…
```

关键点：**主程序侧零改动**。`FindFpgaTool()` 与 `ValidateYosysRuntime()` /
`ValidateNextpnrRuntime()` 本来就按上面这套布局查找，工具落位后即可直接使用。

> 实现细节：CMake 4 已移除单参 `FetchContent_Populate()`，因此这里用
> `SOURCE_SUBDIR` 指向一个不存在的子目录 —— `FetchContent_MakeAvailable()` 找不到
> `CMakeLists.txt` 就只下载、不 `add_subdirectory`，而 `<name>_SOURCE_DIR` 仍指向源码根。
> 这样第三方工程不会污染本工程的配置与编译选项。

## 依赖

**Boost 与 Eigen3 不需要系统安装** —— nextpnr 0.11 对它们是硬要求
（`find_package(Boost REQUIRED COMPONENTS program_options iostreams)`、
`find_package(Eigen3 REQUIRED NO_MODULE)`），且没有内置回退，因此由本模块
用 FetchContent 获取、本地编译，再用 `BOOST_ROOT` / `Boost_DIR` / `Eigen3_DIR`
把位置告知 nextpnr。

| 组件 | Linux | Windows |
|---|---|---|
| 构建工具 | `make`、`gcc/g++`、`bison`、`flex`、`gperf`、`pkg-config` | **MinGW / MSYS2**（`mingw32-make`、`gcc`）；**yosys 不支持 MSVC** |
| yosys | `readline`、`tcl`、`zlib`、`libffi` | 由 MSYS2 包提供 |
| nextpnr | **无需系统 Boost/Eigen**（由本模块 FetchContent 提供） | 同左 |
| apicula | `python3`（纯 Python，`numpy`/`msgspec` 由 venv 安装） | `python3` |

构建顺序（CMake 自动串起来）：

```
Boost(仅 program_options+iostreams) ─┐
Eigen3 ─────────────────────────────┼─▶ nextpnr-himbaechel
                                    │
yosys ────────────▶ yosys-abc       │
apicula ────────────────────────────┘
```

缺失依赖会在 configure 阶段给出明确报错，而不是留到构建中途。

## 版本固定

两端必须用同一套源码，因此 tag 只在一处定义（`cmake/FpgaTools.cmake`），可用 cache 变量覆盖：

| 变量 | 默认 | 说明 |
|---|---|---|
| `SIGFLOW_YOSYS_REPO` / `SIGFLOW_YOSYS_TAG` | `YosysHQ/yosys` / `v0.47` | yosys 源码与版本 |
| `SIGFLOW_YOSYS_SUBMODULES` | `abc` | yosys 的 `abc` 子模块（提供 `yosys-abc`） |
| `SIGFLOW_NEXTPNR_REPO` / `SIGFLOW_NEXTPNR_TAG` | `YosysHQ/nextpnr` / `nextpnr-0.7` | nextpnr 源码与版本 |
| `SIGFLOW_APICULA_REPO` / `SIGFLOW_APICULA_TAG` | `YosysHQ/apicula` / `0.32` | apicula（`gowin_pack`） |
| `SIGFLOW_FPGA_TOOLS_URL_PREFIX` | 空（=GitHub） | 镜像前缀 |
| `SIGFLOW_FPGA_TOOLS_JOBS` | 自动 | 工具链并行编译任务数 |
| `SIGFLOW_YOSYS_SHALLOW` | `OFF` | yosys 是否浅克隆（快，但浅克隆+子模块在个别 git 版本上不稳） |
| `SIGFLOW_FETCH_NEXTPNR_DEPS` | `ON` | 由本模块 FetchContent 提供 Boost/Eigen3（建议保持 ON） |
| `SIGFLOW_BOOST_VERSION` | `1.85.0` | Boost 版本 |
| `SIGFLOW_BOOST_URL` | GitHub release | Boost 源码包 URL（可换内网镜像，也支持 `file://` 本地包） |
| `SIGFLOW_BOOST_LIBRARIES` | `program_options;iostreams` | **只编译 nextpnr 用到的子库**，不编译整套 Boost |
| `SIGFLOW_EIGEN_REPO` / `SIGFLOW_EIGEN_TAG` | `gitlab.com/libeigen/eigen` / `3.4.0` | Eigen3 源码与版本 |
| `SIGFLOW_FPGA_RUNTIME_DIR` | `external/fpga-tools/runtime` | 落位目录 |

## nextpnr 的目标架构

默认构建 **himbaechel + gowin**（对应 Tang Nano 9K）：

```
-DARCH=himbaechel -DHIMBAECHEL_UARCH=gowin -DBUILD_GUI=OFF -DBUILD_PYTHON=OFF
```

gowin 的 `chipdb-*.bin` 由 nextpnr 在编译期借助 **apycula** 生成，因此构建顺序上
apicula 与 nextpnr 都不可少。构建完成后 `cmake/FpgaToolsStageChipdb.cmake` 会把
chipdb 收拢到主程序固定查找的 `nextpnr/share/himbaechel/gowin/`。

## 已知限制

- **构建耗时**：yosys + nextpnr 首次编译通常需要 15–45 分钟，因此默认**不**挂到主目标上；
  需要每次构建都检查时可开 `SIGFLOW_FETCH_FPGA_TOOLS_AS_DEPENDENCY=ON`。
- **网络**：下载发生在 configure 阶段；GitHub 不可达时必须配 `SIGFLOW_FPGA_TOOLS_URL_PREFIX`。
- **Boost 下载较大**（release 包约 130 MB）。内网/慢链路可先自行下载该包，
  再用 `-DSIGFLOW_BOOST_URL=file:///路径/boost-1.85.0-cmake.tar.xz` 指向本地文件。
- **GitHub 的 git 协议若不通**（`git ls-remote` 超时），本模块用的都是 URL/tarball 方式
  （`codeload.github.com` / 官方站点），不依赖 git；也可用
  `-DFETCHCONTENT_SOURCE_DIR_<名称>=<本地源码目录>` 直接喂已下好的源码，彻底离线构建。
- **`openFPGALoader` 已纳入**（含自建 **libftdi1**）。
  Tang Nano 全系在 `src/board.hpp` 里定义为 `cable "ft2232"`（FTDI 线缆），
  而 openFPGALoader 只要 `ENABLE_FTDI_BASED_CABLE` 为 ON 就**强制** `USE_LIBFTDI=ON`
  （普通变量，`-D` 覆盖不掉），FTDI 线缆也没有 libusb 回退实现 ——
  所以 libftdi1 由本模块用 URL 包自建到 `runtime/deps/libftdi/`，
  并给 openFPGALoader 写入 `INSTALL_RPATH`，运行期靠它找到 `libftdi1.so.2`。
  相关 cache 变量：`SIGFLOW_FETCH_OPENFPGALOADER`、`SIGFLOW_OPENFPGALOADER_URL`、
  `SIGFLOW_LIBFTDI_URL`。
- **`verilator` 仍未纳入**（RTL 仿真用），目前沿用以"随包预置或 PATH"的方式。
- **CMake 4 与老工程**：libftdi 1.5 仍写 `cmake_minimum_required(VERSION 2.6)`，
  而 CMake 4 已移除对 <3.5 的兼容，会报
  `Compatibility with CMake < 3.5 has been removed`。
  模块里已按官方逃生口传 `-DCMAKE_POLICY_VERSION_MINIMUM=3.5`；将来接入其它老工程时同理。
- **烧录还需要 USB 权限**：访问板载 JTAG 通常要 udev 规则（openFPGALoader 源码里
  带 `99-openfpgaloader.rules`）或以 root 运行。
- apicula 的脚本目录随平台而异（Windows `Scripts/`、Linux `bin/`），
  主程序 `FindFpgaTool()` 两处都会查找。
