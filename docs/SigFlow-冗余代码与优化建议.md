# SigFlow 冗余代码与优化建议

> 检查日期：2026-10-06  
> 检查方式：静态阅读源代码、构建清单、Git 跟踪状态与文件大小；未运行构建或测试。  
> 范围：当前工作区（包含已有的未提交修改与未跟踪文件）。本文是排查记录，不代表任何文件已获准删除。

## 进展（2026-10-06）

- 已为根目录散落的构建产物补充精准 `.gitignore` 规则；原报告所列 6 个孤儿文件已暂存删除。
- 根目录 README 已对齐当前 CMake + GCC 构建入口，移除失效的 MSVC/slang 指南。
- Job 三层收敛已建立[专项方案](SigFlow-Job三层收敛方案.md)、状态/类型映射及旧历史只读加载器。历史加载器现在会隔离字段类型错误，继续读取其他 Job；`job_tests` 测试和 `sigflow` 构建均已通过。GUI 历史面板接入、新执行路径等价验证及旧实现移除仍待完成。

## 结论

项目最大的结构性重复是 Job 执行体系的多代实现并存。插件化后的 `main/` 中还有一批供旧 GUI 使用的 wxWidgets 适配层。它们目前仍参与构建或被调用，不能仅因与 `core/`、`InnerPlugin/` 同名就删除。另有少量基本确认未参与构建的旧代码，以及构建清单、文档和仓库内容的维护冗余。

| 优先级 | 方向 | 当前判断 | 主要收益 |
| --- | --- | --- | --- |
| 高 | 收敛 Job 与进程执行体系 | 迁移期重复，当前仍在使用 | 减少状态机、持久化和取消逻辑的多份维护 |
| 高 | 管理生成物和大文件 | 已确认存在，可按用途分类处理 | 降低仓库体积与误提交风险 |
| 中 | 清理孤儿代码 | 部分候选证据较充分，删除前仍需核验 | 缩小代码检索范围 |
| 中 | 统一构建入口及文档 | 源清单和说明已漂移 | 避免维护两份不一致的构建配置 |
| 中 | 拆分超大模块 | 维护性问题，不等同于死代码 | 降低修改影响范围 |

## 1. Job 体系有三层并存

| 层次 | 入口 | 当前用途 |
| --- | --- | --- |
| 专用综合 Job | [`main/FpgaSynthesisJob.h`](../main/FpgaSynthesisJob.h)、[`main/FpgaSynthesisJob.cpp`](../main/FpgaSynthesisJob.cpp) | 旧 GUI 综合流程及历史 Job 面板 |
| 旧通用 Job | [`main/jobs/JobService.h`](../main/jobs/JobService.h)、[`main/jobs/JobService.cpp`](../main/jobs/JobService.cpp) | 旧工具流程；仍关联 `ToolJobs`、`PlatformProcess`。无调用的 `JobRegistry` 已移除。 |
| 新契约 Job | [`include/eda/api/jobs.hpp`](../include/eda/api/jobs.hpp)、[`core/src/eda-core/JobService.cpp`](../core/src/eda-core/JobService.cpp) | 插件及教育版 Agent Gateway；GUI 新路径可选 |

三者都覆盖 Job 状态、转换、记录或报告等相近职责。例如专用综合 Job 和旧通用 Job 各自定义了状态枚举与 `IsLegalTransition`；新核心服务也有独立状态机和持久化。旧执行器使用 [`main/jobs/PlatformProcess.cpp`](../main/jobs/PlatformProcess.cpp)，新服务通过 `IProcessHost` 使用 [`core/src/eda-platform/`](../core/src/eda-platform/) 中的平台实现。

目前 GUI 的新路径由 `SIGFLOW_USE_JOB_SERVICE=1` 开启，默认仍走旧路径，见 [`main/Composer.cpp`](../main/Composer.cpp) 的 `UseJobService()` 以及 [`main/MainFrame.cpp`](../main/MainFrame.cpp) 中综合、布线、打包和烧录的分支。Gateway 已直接接入新 `IJobService`。因此不能现在删除旧 Job 服务或旧进程执行器。

**建议顺序：**先列出旧/新流程在提交、取消、超时、重试、产物、历史记录和 GUI 展示上的行为差异；以真实工具与项目数据完成等价验证；将 GUI 和历史 Job 面板迁到同一服务；再移除旧实现及其独有适配层。历史 Job 清单的读取与迁移策略须在删除前确定。

## 2. 插件化后的适配层仍有实际用途

[`CMakeLists.txt`](../CMakeLists.txt) 在插件库定义后明确说明，`main/` 中多份 `.cpp` 是迁移期 wxWidgets 适配层。静态统计以下九个适配源文件共约 670 行：

- `main/FpgaYosysScriptGenerator.cpp`
- `main/fpga/ArtifactValidator.cpp`
- `main/fpga/CstValidator.cpp`
- `main/fpga/FpgaConstraint.cpp`
- `main/fpga/FpgaPackService.cpp`
- `main/fpga/FpgaPinData.cpp`
- `main/fpga/NextpnrLogParser.cpp`
- `main/fpga/NextpnrReport.cpp`
- `main/jobs/Sha256.cpp`

它们承担 `wxString` 与插件层标准类型之间的转换，部分还提供路径查找或旧 UI 数据结构。`main/jobs/Sha256.cpp` 已缩到约 11 行，直接转调核心 SHA256；这类薄封装本身收益很小。优先让调用方使用新接口，待最后一个调用点迁走，再删适配层。

相似的双实现还有 [`main/platform/DynamicLibrary.cpp`](../main/platform/DynamicLibrary.cpp) 与 [`core/src/eda-platform/DynamicLibrary.cpp`](../core/src/eda-platform/DynamicLibrary.cpp)。前者仍被旧 `PluginManager` 使用；[`main/Composer.h`](../main/Composer.h) 也明确保留旧插件管理器以支持 DeepSeek 等插件。统一前须先完成旧插件接口迁移。

## 3. 可优先核验的孤儿代码

以下候选未列入当前 CMake 主程序源清单或旧 `main.vcxproj`，静态搜索也未发现有效调用点：

| 候选 | 证据 | 建议 |
| --- | --- | --- |
| [`main/WavePanel.cpp`](../main/WavePanel.cpp) 与 [`main/WavePanel.h`](../main/WavePanel.h) | 约 430 行旧 VCD 面板；当前波形界面使用 `main/wave/` 下的实现，其他匹配多为注释或名称相似 | 确认无外部直接依赖后清理 |
| [`main/SFNodePropertyPanel.cpp`](../main/SFNodePropertyPanel.cpp) 与 [`main/SFNodePropertyPanel.h`](../main/SFNodePropertyPanel.h) | 实现仅有空壳式 `loadNode`，未见实例化 | 确认无计划中的用途后清理 |
| [`main/Command.h`](../main/Command.h)、[`main/SigFlowContext.h`](../main/SigFlowContext.h) | 两文件内容完全相同，均只有 `#pragma once`；未见引用 | 可作为最小清理项 |

**例外：**[`main/Simulation/sc_time_stub.cpp`](../main/Simulation/sc_time_stub.cpp) 虽不作为主程序源文件编译，但 [`SimulationEngine.cpp`](../main/Simulation/SimulationEngine.cpp) 会查找或生成同名源码供仿真工具使用，不能按“未列入 CMake”判为无用。

## 4. 仓库体积与生成物

本次检查中，Git 跟踪文件的工作区大小合计约 **162 MiB**。较大的项目如下（数值为本地文件大小，非 Git 历史占用）：

| 内容 | 约大小 | 处理前需确认 |
| --- | ---: | --- |
| `external.zip`、`3rd.zip`、`tools.zip` | 合计 91.8 MiB | [`docs/BUILD.md`](BUILD.md) 说明克隆后必须解压；保留离线分发能力或提供可靠替代下载源 |
| `external/slang-sdk/svlang.dll`、`svlang.lib` | 合计 32.1 MiB | [`CMakeLists.txt`](../CMakeLists.txt) 称当前不再构建/链接 slang；核验其他脚本、旧工程和分发流程 |
| `examples/tracebridge_tangnano9k/.sigflow/` | 129 个跟踪文件，约 24.2 MiB | 区分可重建运行结果与用于验收的固定样例 |
| 示例工程 `yosys/`、`nextpnr/` | 合计约 3.6 MiB | 确认示例说明、回归测试是否依赖具体产物 |

此外，工作区存在未跟踪的 `DebugContract.obj`、`jsoncpp.obj`、`main/main/`、`out/` 等构建结果；所查几处合计约 16.7 MiB。[`.gitignore`](../.gitignore) 已排除 `build/`、`build-*/`，但未覆盖这些路径。可先补充精准忽略规则，防止误提交；是否移除已跟踪的大文件，须先确认离线构建及示例验收需求。不要用统一通配规则排除所有 `.sigflow`，因为示例中的固定产物可能有测试价值。

## 5. 构建配置与文档已漂移

[`CMakeLists.txt`](../CMakeLists.txt) 使用显式主程序源清单，同时仍跟踪 [`main/main.vcxproj`](../main/main.vcxproj)、`main/main.vcxproj.filters` 和个人配置 `main/main.vcxproj.user`。比较两份主程序源清单，CMake 比 vcxproj 多 8 个 `.cpp`，包括 `agent/AgentServiceController.cpp`、`debug/FtdiTransport.cpp`、`platform/SimToolchain.cpp` 等。该差异足以说明直接维护 vcxproj 容易遗漏新功能；它不意味着这 8 个文件未参与 CMake 构建。

构建说明也互相矛盾：[根目录 README](../README.md) 仍指导使用 Visual Studio 生成器，[`docs/BUILD.md`](BUILD.md) 则写明只支持 CMake + MinGW-w64 GCC、不支持 MSVC。建议先确定当前受支持的构建方式，再同步入口文档；若手写 vcxproj 已不支持，可停止维护并处理个人 `.user` 文件。

## 6. 大模块的维护成本

静态行数显示 [`main/MainFrame.cpp`](../main/MainFrame.cpp) 约 4540 行、[`main/debug/TraceBridgeWindow.cpp`](../main/debug/TraceBridgeWindow.cpp) 约 2533 行、[`core/src/eda-agent-gateway/GatewayServer.cpp`](../core/src/eda-agent-gateway/GatewayServer.cpp) 约 2421 行。它们不是死代码，但集中了 GUI、流程调度、会话处理或路由逻辑。建议结合实际功能改动逐步抽取独立服务/处理器，并保持公开行为与测试覆盖；仅为缩短文件而机械拆分意义有限。

资源目录中 PNG、SVG、`svg_icons` 并存，但按哈希检查未发现完全相同的资源文件。没有足够证据建议批量删除图标。

## 实施顺序与验收点

1. **仓库卫生：**确认离线依赖和示例产物用途，补精准忽略规则，整理文档与仓库存放策略。验收：全新克隆仍能按构建指南准备依赖，示例验收仍可运行。
2. **小范围清理：**逐个核对孤儿代码的构建与调用引用，再移除。验收：主程序构建和波形、属性面板相关功能不回退。
3. **统一构建入口：**以实际支持的工具链为准更新 README、BUILD 指南及旧工程去留。验收：文档中的命令在干净环境可执行。
4. **Job 系统收敛：**先验证新旧执行路径等价及历史记录兼容，再切换 GUI，最后删除旧 Job/Process 实现和不再使用的适配层。验收：综合、布线、打包、烧录、仿真的提交/取消/重试/超时/产物/报告以及 Agent Gateway 均通过回归检查。
5. **模块拆分：**在上述功能稳定后，按职责拆分 `MainFrame`、`TraceBridgeWindow`、`GatewayServer`，避免与 Job 切换同时扩大改动面。

## 检查局限

本报告基于当前工作区的静态证据。工作区原先已有大量未提交修改与未跟踪源文件；本次未运行编译、测试、覆盖率或实际 GUI 操作。因此“孤儿代码”是优先核验候选，尚不是删除结论；行数与体积是本地快照，后续会随工作区变化。
