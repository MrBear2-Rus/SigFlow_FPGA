# Harness P0 API 覆盖清单

> 日期：2026-09-21
> 范围：`include/eda/api/` 最小契约层（本次冻结）
> 依据：`docs/HarnessPlan.md` §4（三层接口）、§6（C ABI）、§7（schema）、§8（Job）
> 设计：`docs/HarnessP0-Api-Contract-Design.md`　计划：`docs/HarnessP0-Api-Contract-Plan.md`

状态图例：**已冻结** = 签名已定并可编译；**下轮** = 未建，应在对应 P0/P1/P2 任务中补齐。

---

## 1. §4.1 第一层 `IPluginInteraction`（`include/eda/api/IPluginInteraction.h`）

| 条目 | 状态 |
| --- | --- |
| `invoke` / `subscribe` / `unsubscribe` / `info` / `onLoad` / `onUnload` | 已冻结 |
| `MethodCall{method, params, requestId}`（`Types.h`） | 已冻结 |
| `PluginInfo{...abi, buildInfo}`（`Types.h`） | 已冻结 |
| `Error` / `ErrorCode` / `Callback` / `EventHandler` / `Json`（`Types.h`） | 已冻结 |
| `Service` 基类 | 已冻结（P0-2 迁移至 `services.hpp`） |

## 2. §4.2 第二层能力接口（`include/eda/api/capabilities.h`）

| 接口 | 状态 |
| --- | --- |
| `EDA_TYPED_METHOD` 宏 | 已冻结 |
| `ISynthesizer`（`synthesizerId` / `synth_run`）+ `SynthParams`/`SynthResult` | 已冻结（示范） |
| `IHDLFrontend` | 下轮（P1，tree-sitter 插件化） |
| `ISimulator` | 下轮（P1-4） |
| `IPlaceAndRouter` | 下轮（P1-2） |
| `IPacker` | 下轮（P1-3） |
| `IProgrammer` | 下轮（P1-3） |
| `IDesignIR` | 下轮（P2-1） |
| `IWaveformBackend` | 下轮（P1-9） |
| `ILlmProvider` | 下轮（P3-2/P3-3） |
| `IMcpClient` | 下轮（P3-4） |
| `IComponentLibrary` | 下轮（P2-5） |

## 3. §6.1 C ABI（`include/eda/api/abi.h`）

| 条目 | 状态 |
| --- | --- |
| `EDA_PLUGIN_ABI_VERSION` | 已冻结 |
| `eda_plugin_descriptor_v1`（含 `build_info`） | 已冻结（仅结构体布局） |
| `eda_host_api_v1` | 仅前向声明；实现下轮（P0-4） |
| `eda_plugin_query_v1` | 已声明 + 动态加载已实现（`PluginHost`，P0-3） |
| 构建指纹校验（宿主/插件 `build_info` 比对） | 已实现（`PluginHost::LoadAll`，P0-3；修 R13） |
| 进程外 JSON-RPC / 共享内存（§6.2） | 下轮（P3-1） |

## 4. §7.3 冻结 schema

| schema id | 状态 |
| --- | --- |
| `eda.plugin-manifest.v1` | 下轮（P0-3） |
| `eda.job.v1` | 已注册（`RegisterCoreSchemas`，P0-5） |
| `eda.jobreport.v1` | 已注册（`RegisterCoreSchemas`，P0-5）；`CoreJobService` 产出该结构 |
| `eda.target-profile.v1` | 下轮（P1-5） |
| `eda.ir.*.v1` | 下轮（P2-1） |
| `eda.netlist.yosys-json.v1` | 下轮（P1-1） |

## 5. §8.2 Job 抽象（`include/eda/api/jobs.hpp`）

| 条目 | 状态 |
| --- | --- |
| `JobState`（9 态） | 已冻结 |
| `JobRequest` / `JobTransition` / `JobRecord` / `JobReport` | 已冻结；`JobRequest` 增 `timeoutSec`（P0-5 契约微调） |
| `IJobProvider` | 已冻结；`startJob` 返回 `Error`（P0-5 契约微调） |
| `IJobService` | 已冻结 + 已实现（`CoreJobService`，P0-5） |
| `JobContext` | 已冻结 + 已实现（`CoreJobContext`，P0-5） |
| `JobContext::processHost()` | 已实现（懒创建 `IProcessHost`，接 `eda_platform`） |
| §8.3 统一状态迁移表 | 已实现（`CoreJobService::IsLegalTransition`） |
| `IJobService::cancel` 真杀进程树 / `timeoutSec` 生效 | 已实现（`CoreJobContext::RequestCancel` → `IProcessHost::Cancel`；超时看门狗） |

## 6. 其他公共头

| 头 | 状态 |
| --- | --- |
| `services.hpp`（`Service` / `ServiceRegistry` / `Context`） | 已冻结（P0-2） |
| `events.hpp`（`EventBus` + §9.3 主题常量） | 已冻结（P0-2） |
| `schemas.hpp`（`ISchemaRegistry`） | 已冻结（P0-2；子集校验，完整 schema 替换实现下轮） |
| `process.hpp`（`IProcessHost` / `ProcessSpec` / `ProcessResult`） | 已冻结（P0-4） |
| `PluginHost`（发现/校验/状态机/诊断/能力选择） | 已实现（`core/src/eda-core/PluginHost.*`，P0-3；修 R1/R2/R13） |
| `toolchain.hpp`（`IToolchain` / `ToolQuery` / `ToolResolution`） | 已冻结 + 已实现（`DefaultToolchain`，P0-6；修 R19） |
| `project.hpp`（`IProject`） | 已冻结 + 已实现（`JsonProject`，P0-6；修 R9/R17） |
| `plugin_registry.h`（`EDA_REGISTER_PLUGIN`） | 已冻结 + 已实现（`PluginRegistry.cpp`，P0-7） |
| `text_document.hpp`（`ITextDocument`） | 下轮（P2-4） |

---

## 7. 冻结与实现记录

- 冻结头文件（P0-1）：`Types.h`、`IPluginInteraction.h`、`capabilities.h`、`jobs.hpp`、`abi.h`、`build_info.h`。
- 冻结头文件（P0-2）：`services.hpp`、`events.hpp`、`schemas.hpp`；`Service` 基类自 `Types.h` 迁至 `services.hpp`。
- 冻结头文件（P0-4）：`process.hpp`（`IProcessHost` / `ProcessSpec` / `ProcessResult` / `ProcessOutcome` / `CreatePlatformProcessHost`）。
- 内核骨架：`core/src/eda-core/Logger.{h,cpp}`、`Version.cpp`（P0-1）+ `EventBus.{h,cpp}`、`SchemaRegistry.{h,cpp}`（P0-2）；`core/CMakeLists.txt`。
- 平台层（P0-4）：`eda_platform` STATIC + `core/src/eda-platform/{ProcessCollector.h, win32/PlatformProcessHost.cpp, posix/PlatformProcessHost.cpp}`。
  Windows 后端沿用既有 `PlatformProcess`（wx 版）已验证的关键修复：进程创建串行化、`CREATE_SUSPENDED`→AssignJobObject→`ResumeThread`、`KILL_ON_JOB_CLOSE`、独立 stdout/stderr、超时/取消 `TerminateJobObject`。
  POSIX 后端按同语义移植（`fork/exec`、`setpgid/killpg`、`FD_CLOEXEC`、非阻塞 `poll`）；**本机为 Windows，POSIX 分支未编译，待 Linux 真机验证**。
- Job 服务（P0-5）：`core/src/eda-core/JobService.{h,cpp}`（`CoreJobService` / `CoreJobContext`）、`CoreSchemas.{h,cpp}`。
  异步 worker 池、`RegisterProvider`、`get/list/report`、`retry`、`setConcurrency`、文件 manifest/job-report 持久化（默认落临时目录）。
  取消/超时经 `CoreJobContext::RequestCancel` → `IProcessHost::Cancel` 真杀进程树；超时看门狗在子线程运行 `startJob`。
  **契约微调（P0-5）**：`IJobProvider::startJob` 由 `void` 改为返回 `Error`（失败置 Failed）；`JobRequest` 增 `std::optional<int> timeoutSec`（覆盖服务默认超时）。
- 插件宿主（P0-3）：`core/src/eda-core/PluginHost.{h,cpp}` + `core/src/eda-platform/DynamicLibrary.{h,cpp}`。
  静态注册（`RegisterStatic`）+ 动态加载（`eda_plugin_query_v1` + ABI/`build_info` 校验）+ 状态机 + 结构化诊断；按能力选择（`ProvidersOf`/`Select`）。
  搜索链：`AddSearchPath` / `AddDefaultSearchPaths`（bundled→用户→项目）/ `SIGFLOW_PLUGIN_PATH`。失败绝不静默（修 R1），选择不再按名字硬编码（修 R2），构建指纹不匹配拒绝加载（修 R13）。
  测试用 MODULE 插件：`tests/contract/test_plugin_{ok,bad_build,reject_abi}.cpp`。
- 工具与工程（P0-6）：`core/src/eda-core/Toolchain.{h,cpp}`（`DefaultToolchain`：配置→随包→环境变量→PATH 四层；未找到必带 reason，修 R19）、`Project.{h,cpp}`（`JsonProject`：读写真实 `sigflow.project` 格式、保留未知段、临时文件 + 原子替换，修 R9/R17）。
- 插件注册与构建原语（P0-7 核心）：`include/eda/api/plugin_registry.h` + `core/src/eda-core/PluginRegistry.cpp`（`EDA_REGISTER_PLUGIN` 自注册 → `PluginHost::RegisterBuiltins`）；`cmake/EdaPlugin.cmake` 增 `KIND=MODULE` 分支，测试用插件已改用 `eda_add_plugin` 构建。
- 构建原语：`cmake/EdaPlugin.cmake`（`eda_add_plugin`，支持 `KIND=STATIC|MODULE`）。
- 构建接线：根 `CMakeLists.txt` 定义 `eda_api`(INTERFACE) + `add_subdirectory(core)`；`tests/CMakeLists.txt` 增 `SIGFLOW_BUILD_CONTRACT_TESTS` → `tests/contract`。
- main 侧改造（P0-7/P0-8/P0-9，均已完整构建验证）：
  - **P0-7（R16）**：根 `CMakeLists.txt` 以 101 条**显式源列表**取代 `file(GLOB_RECURSE main/*.cpp)`；`main.vcxproj` / `.filters` 同步（新增 `Composer.*`、移除 `PluginStore.*`）。
  - **P0-8**：新增组合器骨架 `main/Composer.{h,cpp}`，集中插件的 legacy 加载（`PluginManager`/`ISigPlugin`）+ 新契约加载（`eda::PluginHost`：`RegisterBuiltins`/发现/校验/诊断），`MainFrame` 改为经 `Composer` 使用（旧 DeepSeek 路径保留，迁移留 P3）。
  - **P0-9**：删除 `main/PluginStore.{h,cpp}`，并移除工程文件条目。
  - **P0-6 深化**：`MainFrame::FindFpgaTool` 改为调用 `eda::DefaultToolchain`（配置→随包 runtime→env→PATH 四层，随包按 `yosys/bin`、`nextpnr/bin`、`apicula/Scripts`、`apicula/bin`、`openfpgaloader/bin` 子目录；未找到返回空串保持旧契约）。
- 构建接线（主程序）：`sigflow` 目标新增 `target_link_libraries(PRIVATE eda_core)` 与 `core/src` include（供 `Composer` 使用 `eda-core/PluginHost.h`）。
- 验收证据：`eda_api_contract_smoke` 直编（`-Wall -Wextra`，零警告）+ CMake 构建 + `ctest` 均 PASS；**完整 `sigflow.exe` 构建成功**。

## 8. 后续建议顺序

1. `PluginHost` 补 `eda_host_api_v1` 实现，使动态插件可经宿主 API 回调（当前 `register_plugin` 传 `nullptr`）。
2. P3：把 DeepSeek 从 legacy `ISigPlugin` 迁移到 `IPluginInteraction`，删除 `main/PluginManager.*`，`Composer` 全面切到 `PluginHost`。
3. `JsonProject` 接入"新建 `.v` 自动写入 `sigflow.project`"（当前 `wxID_NEW` 创建整个工程并已写好 `source_files`，无独立新增文件流程）。
4. P0-7 收尾：wx 归一（`3rd/wxWidgets-3.2.9` vs 源码构建）与 JSON 归一（jsoncpp vs nlohmann）。
5. `PlatformProcess`（wx 版）/ `YosysExecutor` / `NextpnrExecutor` 的进程管理向 `eda_platform` 收敛（修 R4）。
6. 完整 JSON Schema 校验实现；Linux 真机编译验证 POSIX 后端与 MODULE 插件（`.so`）。

---

## 9. P0 验收汇总

| 测试目标 | 覆盖 | 结果 |
| --- | --- | --- |
| `eda_api_contract_smoke` | 契约类型/接口/Job 结构 + ServiceRegistry/EventBus/SchemaRegistry + `IProcessHost`（真进程/超时/取消） | ALL PASS（36） |
| `eda_job_service_smoke` | `CoreJobService`：成功/异常失败/无 provider/取消杀进程/超时、`RegisterCoreSchemas` | ALL PASS（13） |
| `eda_plugin_host_smoke` | 静态注册/能力选择/重复拒绝；真实加载 3 个插件：Ready / 指纹拒绝 / 握手拒绝；诊断报告 | ALL PASS（17） |
| `eda_toolchain_project_smoke` | `DefaultToolchain` 四层发现 + 子目录 + NotFound reason；`JsonProject` 加载/原子保存/幂等增删/未知段保留；`EDA_REGISTER_PLUGIN` | ALL PASS（23） |

- 全部直编 `g++ -std=c++20 -Wall -Wextra` 零警告；`ctest -R eda_` **4/4 Passed**。
- **完整应用构建**：`cmake --build build-gcc --target sigflow` 成功（`sigflow.exe` 链接通过）。
- P0-1…P0-9 已完成：P0-1 契约、P0-2 服务、P0-3 插件宿主、P0-4 进程抽象、P0-5 Job 服务、P0-6 工具/工程 + `FindFpgaTool` 接入、P0-7 源集合对齐/BUILD 原语/静态注册、P0-8 Composer 骨架、P0-9 死代码清理。
- 明确遗留（见 §8）：wx/JSON 归一、legacy 插件迁移、`eda_host_api_v1` 回调、POSIX 真机验证。
