# SigFlow 跑在 Linux / 麒麟系统上的可行性调研

> 调研日期：2026-09-02
> 结论先行：**可行，且比预想的乐观**。真正的瓶颈不在工具链（Yosys/nextpnr 等本来就是 Linux 原生的开源项目），而在 SigFlow IDE 自身约 18 个文件的 Windows API 依赖和 MSVC 构建体系——这部分是清晰、有限、可逐步替换的。分架构看：**x86_64 Linux / 麒麟 ≈ 直接可迁移；ARM64（飞腾/鲲鹏）= 需自行编译工具链，可行；LoongArch（龙芯）= 风险最大，需实测验证**。

---

## 1. 现状体检：SigFlow 的 Windows 依赖到底有多深

对 `main/` 全部源码做了静态扫描，结论：

| 层 | 现状 | 跨平台性 |
|---|---|---|
| GUI 框架 | **wxWidgets 3.2.9** | ✅ 本身就是跨平台框架（Windows/Linux/macOS），Linux 上走 GTK 后端，麒麟可用 |
| 第三方库 | jsoncpp、nlohmann/json、nanosvg、tree-sitter(+verilog)、VCD 解析 | ✅ 全部纯 C/C++，官方支持 Linux |
| 业务逻辑 | SFTree、Job 状态机、报告解析、波形数据层 | ✅ 标准 C++，无平台依赖 |
| **平台相关代码** | **18 个文件** include `windows.h` | ⚠️ 需要适配，但集中在四类（见下） |
| 构建体系 | 只有 `SigFlow.sln`（MSVC/Visual Studio），无 CMake/Makefile | ⚠️ 需要新建一套构建 |
| 捆绑工具链 | `external/fpga-tools/runtime/` 全是 Windows `.exe` | ⚠️ Linux 上需换 Linux 版工具链（好消息：见第 2 节） |

18 个平台相关文件分四类，每类在 Linux 上都有标准等价物：

1. **进程执行**（`FpgaYosysExecutor`、`NextpnrExecutor`、`ProcessRunner` 等）：Win32 `CreateProcess` + Job Object（进程树清理）→ Linux 用 `fork/exec` + 进程组（`setpgid` + `killpg`），管道收日志同理；
2. **串口**（`SerialTransport`/`SerialPortEnumerator`）：Win32 COM 口 → Linux `termios` + `/dev/ttyUSB*`，这部分是教科书级标准操作；
3. **命名管道**（`PipeTransport`，无硬件 CI 用）→ Unix FIFO 或 Unix domain socket；
4. **杂项**：WGL OpenGL 上下文（→ GTK/GLX，wxWidgets 已封装好）、文件路径分隔符、指纹计算里的 Win32 文件 API。

**关键有利因素**：我们的 Job 化框架已经把"调工具"隔离在 Executor/Runtime 层后面，MainFrame 和业务层不直接碰进程 API——这意味着移植面是**小而有边界的**，不是满屏 `#ifdef`。

## 2. 底层工具链：Linux 上比 Windows 更"原生"

Yosys、nextpnr、Apicula（gowin_pack）、openFPGALoader 全部诞生于 Linux 社区，Tang Nano 9K 的完整开源流程（综合→布线→打包→烧录）在 Linux 上有大量公开教程验证过：

- **YosysHQ 官方 oss-cad-suite**：提供 linux-x64 一站式打包（Yosys + nextpnr-himbaechel + openFPGALoader + Verilator，版本互相匹配），解压即用，与我们现在的"捆绑 runtime"思路完全一致；
- **发行版软件源**：Ubuntu/Debian 仓库里有 yosys、openfpgaloader；apycula 已进入 Debian 官方源（意味着自动支持多架构）；
- **源码编译**：nextpnr-himbaechel 用 CMake 一条命令开 Gowin 支持（`-DARCH=himbaechel -DHIMBAECHEL_UARCH=gowin`），依赖只有 Boost/Eigen/Python，无厂商闭源组件；
- **Verilator**：本来就是 Linux 一等公民，我们的仿真链路在 Linux 上只会更顺；
- **USB 烧录**：openFPGALoader 走 libusb/FTDI 标准驱动，Linux 上配一条 udev 规则即可（比 Windows 驱动问题还少）。

也就是说：**工具链层零风险，直接换 Linux 版本即可**，我们的 Runtime 发现链（项目路径 → 捆绑 runtime → 环境变量 → PATH）不需要改设计。

## 3. 麒麟系统的实际情况

银河麒麟 V10 是 Linux 内核操作系统（桌面版与 Ubuntu/优麒麟同源），**同源支持 x86_64、ARM64（飞腾/鲲鹏）、LoongArch（龙芯）、申威、兆芯、海光** 等架构，使用 deb 包体系，官方宣称兼容 90% 以上主流开源软件。对我们的分架构评估：

| 架构 | 工具链可用性 | SigFlow IDE | 总体评估 |
|---|---|---|---|
| **x86_64**（Intel/AMD/兆芯/海光） | oss-cad-suite linux-x64 直接可用 | wxWidgets/GTK 齐备 | 🟢 **低风险，可优先落地** |
| **ARM64**（飞腾/鲲鹏） | yosys 有发行版包；nextpnr/gowin_pack 需源码编译（CMake 流程成熟）；apycula 是纯 Python 无架构限制 | wxWidgets 支持 ARM64 Linux | 🟡 **可行，多一道"自己编工具链"的工序** |
| **LoongArch**（龙芯） | 社区已有先例（Drink-EDA 项目在龙芯上打包了 yosys；有人早期在龙芯上编译过 yosys/icestorm），但 nextpnr-himbaechel + Gowin chipdb 无公开验证；另有 x86 二进制翻译兜底（官方称可跑大部分 x86 Linux EDA 软件，性能损失约 20%） | 理论可行，需实测 | 🔴 **不确定，必须拿实机验证后再下结论** |

## 4. 如果要做，工作量拆解

**第一步（验证性，1~2 周）**：在 Ubuntu x86_64 上跑通"命令行级"全流程——下载 oss-cad-suite，用我们示例工程 `examples/tracebridge_tangnano9k` 的同一套参数（Yosys 脚本、nextpnr 参数、gowin_pack 命令原样照搬）在 Linux 上综合→布线→打包→烧录→UART 采集。**这一步不需要改任何 SigFlow 代码**，就能证明"底座"完全成立，也是给导师的最有力证据。

**第二步（IDE 移植，1~2 个迭代）**：
1. 新建 CMake 构建体系（先 Linux，后可反哺 Windows，摆脱对 Visual Studio 工程文件的单一依赖）；
2. 抽一层薄的平台抽象（进程/串口/管道约 4 个接口，POSIX 实现）；
3. 路径分隔符与文件 API 清理；
4. debug CI 从 PowerShell 脚本改为跨平台脚本（bash）。

**第三步（麒麟落地）**：x86_64 麒麟装机验证（理论上与 Ubuntu 无本质差异）；ARM64 麒麟需在目标机上源码编译工具链并捆绑；龙芯视实机测试结果决定投入。

## 5. 结论与建议

1. **"希望渺茫"的直觉可以推翻**：难点不在生态（麒麟兼容主流开源软件，工具链 Linux 原生），而在我们自己代码的 Windows 依赖——而这部分经扫描是**有限且边界清晰的**（18 个文件、4 类问题），且 Job 化框架的分层设计恰好把移植面收窄到了 Executor/传输层。
2. **建议按"x86_64 Linux → x86_64 麒麟 → ARM64 麒麟 → 龙芯"的顺序推进**，每一步都有明确的验收物；第一步成本极低、说服力极强。
3. **顺带的收益**：迁移到 CMake + 跨平台构建后，项目还能获得 macOS 支持、更干净的 CI（当前 CI 已有 Windows GitHub Actions，可加 Linux job），对后续 Agent 化/教育版进高校机房（不少高校实验室是国产系统）也有实际意义。
4. **需要提前确认的事**：目标麒麟机器的具体 CPU 架构（问导师/装机单位），这直接决定走哪条分支；若是龙芯，建议先申请一台实机做第 1 步验证。

---

*调研依据：SigFlow 源码静态扫描（2026-09-02）；YosysHQ oss-cad-suite 与 Apicula/nextpnr 官方文档；银河麒麟 V10 官方产品资料；Debian/Ubuntu 软件源；龙芯社区公开移植案例（Drink-EDA 等）。*
