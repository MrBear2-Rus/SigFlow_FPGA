# Harness P1 进展（工具链插件化）

> 日期：2026-09-21
> 基线：`SigFlow_FPGA_Cmake`（P0 已完成，见 `docs/HarnessP0-Gate-Review.md`）
> 依据：`docs/HarnessPlan.md` §13 P1（P1-1…P1-9）

---

## P1-1 `eda-synth-yosys`（已完成）

> 范围依据 HarnessPlan §13 P1-1：**脚本生成器 / ArtifactValidator 移入；输出走 jobDir**。两项均已移入并去 wx 化，P1-1 达成。
> `FpgaYosysLogParser`/`FpgaYosysReport` 不在 P1-1 范围内（属 UI 报告侧，另行处理）。

### 已完成（第一增量）

- 新增官方插件目录 `InnerPlugin/eda-synth-yosys/`：
  - `edaplugin.yml`（`location: inner`、`capabilities: [synth/yosys]`、`jobs: [synth]`）
  - `CMakeLists.txt`：`eda_add_plugin(eda-synth-yosys KIND STATIC ...)`
  - `include/YosysScriptGenerator.h` + `src/YosysScriptGenerator.cpp`：**脚本生成逻辑自 `main/FpgaYosysScriptGenerator` 移入并去 wx 化**（`std::string`/`std::filesystem`，请求字段改为 `targetProfileId/Version/yosysFamily` 字符串以解耦 wx 版 `FpgaTargetProfile`）
  - `include/YosysSynthesizer.h` + `src/YosysSynthesizer.cpp`：同时实现 `eda::ISynthesizer` 与 `eda::IJobProvider`（`jobType=synth`）；经 `JobContext::processHost()` 跑 `yosys -s script.ys`，校验产物并 `registerArtifact`；`EDA_REGISTER_PLUGIN(YosysSynthesizer, eda::synth::YosysSynthesizer, "eda-synth-yosys")`
- `main/FpgaYosysScriptGenerator.{h,cpp}` 保留为**薄 wx 适配层**（同签名转发到插件），保证旧调用点（`MainFrame`、`fpga_flow_probe`）在迁移期不破。
- 构建接线：根 `CMakeLists.txt` 增 `add_subdirectory(InnerPlugin/eda-synth-yosys)`；`sigflow` 链接 `eda-synth-yosys` + 插件 include。
- 测试：`tests/plugins/eda_synth_yosys_smoke.cpp`
  - `EDA_REGISTER_PLUGIN` → `PluginHost::RegisterBuiltins` 能发现 `eda-synth-yosys`、暴露 `synth/yosys`
  - 经 `CoreJobService` + 假 `IProcessHost` 跑通：提交 → 执行 → 产物 → 报告（Succeeded、`netlist` artifact、`exit_code` metric）

### 验收

- 直编 `g++ -std=c++20 -Wall -Wextra` 零警告；`eda_synth_yosys_smoke` **ALL PASS（8）**
- `ctest -R eda_` **5/5 Passed**
- **完整应用构建** `cmake --build build-gcc --target sigflow` 成功（插件/adapter 均链接）

### 已完成（第二增量）

- `ArtifactValidator` 自 `main/fpga/ArtifactValidator` **移入插件并去 wx 化**：
  - `InnerPlugin/eda-synth-yosys/{include/YosysArtifactValidator.h, src/YosysArtifactValidator.cpp}`（`std::filesystem` + `nlohmann` + `eda_platform` SHA-256）
  - `main/fpga/ArtifactValidator.cpp` 保留为**薄 wx 适配层**（同签名，双向转换报告），`MainFrame`/`fpga_flow_probe` 零改动
- SHA-256 自 `main/jobs/Sha256` **移入 `core/src/eda-platform/Sha256.{h,cpp}`**（std 版）；`main/jobs/Sha256.cpp` 改为转发，wx API 不变
- `YosysSynthesizer::startJob` 改用移入的 `ArtifactValidator` 做**真实产物校验**（含 `sha256`、`ports/cells/netnames` 指标），校验失败 → Job `Failed`
- 测试扩展 `eda_synth_yosys_smoke`：合法网表 → `Succeeded`+artifact+sha256+cell 指标；非法网表 → `Failed`

### 验收

- `eda_synth_yosys_smoke` **ALL PASS（10）**；`ctest -R eda_` **5/5 Passed**
- **完整应用构建** `cmake --build build-gcc --target sigflow` 成功（插件 + 两个 wx 适配层）

### 本增量未做（后续）

- `FpgaYosysLogParser`、`FpgaYosysReport` 移入插件并去 wx 化（下个增量；`FpgaSynthesisJobsPanel` 亦引用日志解析，需同步适配）。
- UI 经 `IJobService` 提交综合（P1-8）——当前 `MainFrame` 仍走 legacy `main/jobs`。

## P1-2 `eda-pnr-nextpnr`（进行中）

### 已完成（第一增量）

- 契约扩展：`include/eda/api/capabilities.h` 新增 `IPlaceAndRouter`（`pnrId()` + `EDA_TYPED_METHOD(pnr_run, PnrParams, PnrResult)`）。
- 新增官方插件 `InnerPlugin/eda-pnr-nextpnr/`：
  - `edaplugin.yml`（`capabilities: [pnr/nextpnr]`、`jobs: [pnr]`）
  - `include/NextpnrPlaceRouter.h` + `src/NextpnrPlaceRouter.cpp`：实现 `IPlaceAndRouter` + `IJobProvider`；经 `IProcessHost` 跑
    `nextpnr-himbaechel --json <netlist> --write <out> --device <dev> --vopt family=<fam> [--vopt cst=<cst>]`；
    默认输出 `jobDir/artifacts/<top>.pnr.json`；轻量工具发现经 `DefaultToolchain`（`SIGFLOW_NEXTPNR`/PATH）；
    `EDA_REGISTER_PLUGIN(NextpnrPlaceRouter, eda::pnr::NextpnrPlaceRouter, "eda-pnr-nextpnr")`
- 测试 `tests/plugins/eda_pnr_nextpnr_smoke.cpp`：注册表可见 + `pnr/nextpnr`；经 `CoreJobService` + 假 `IProcessHost` 跑通，产物 `pnr-json`

### 验收

- `ctest -R eda_` **6/6 Passed**（含 `eda_synth_yosys_smoke`、`eda_pnr_nextpnr_smoke`）
- **完整应用构建** 成功（`sigflow.exe`）

### 本增量未做（后续）

- `NextpnrLogParser` / `NextpnrReport` / `CstValidator` 移入插件并去 wx 化（下个增量）。
- UI 经 `IJobService` 提交布线（P1-8）；当前 `MainFrame` 仍走 legacy `main/fpga/NextpnrJob*`。

### 已完成（第二增量）

- `NextpnrLogParser`（469 行）与 `NextpnrReport`（254 行）**移入插件并去 wx 化**（`std::regex`/`std::string`/`std::vsnprintf`）：
  - 插件 `InnerPlugin/eda-pnr-nextpnr/{include/NextpnrLogModel.h, include/NextpnrReportGen.h, src/NextpnrLogParser.cpp, src/NextpnrReport.cpp}`
  - 头文件重命名（`NextpnrLogModel.h`/`NextpnrReportGen.h`）以避开与 `main/fpga/Nextpnr*` 的同名包含歧义
  - `main/fpga/NextpnrLogParser.cpp` / `NextpnrReport.cpp` 保留为**薄 wx 适配层**；转换集中在 `main/fpga/NextpnrRecordBridge.h`；`NextpnrExecutor`/`MainFrame` 零改动
- `sigflow` 链接 `eda-pnr-nextpnr` + 插件 include。
- 测试扩展 `eda_pnr_nextpnr_smoke`：日志解析（三阶段 reached、时序 62.64MHz PASS、资源 IOB、版本 0.7）+ 报告（summary/json）。

### 验收

- `ctest -R eda_` **6/6 Passed**；`eda_pnr_nextpnr_smoke` 覆盖 PnR Job + 解析/报告
- **完整应用构建** 成功（`sigflow.exe`，含两个 wx 适配层）

### 本增量未做（后续）

- `CstValidator` 移入：依赖 `FpgaConstraint`（691 行）与 `FpgaPinData`，按计划与 **P1-6 `eda-cst-gowin-cst`** 一并移入。
- UI 经 `IJobService` 提交布线（P1-8）。

## P1-3 `eda-pack-gowin` / `eda-program-openfpgaloader`（第一增量完成）

- 契约扩展：`capabilities.h` 新增 `IPacker`（`pack_run`）与 `IProgrammer`（`flash_run` + `requiresConfirm()`）；`IJobProvider` 增 `virtual bool requiresConfirm() const { return false; }`。
- `CoreJobService` 落地**确认门控**：`provider->requiresConfirm() && !request.requireConfirm` → 直接 `Failed`。
- 新增插件：
  - `InnerPlugin/eda-pack-gowin/`（`GowinPacker`）：`gowin_pack -d <device> -o <fs> <pnr.json>`，注册 `bitstream` 产物（schema `eda.bitstream.apicula.v1`）；`capabilities: [pack/gowin]`。
  - `InnerPlugin/eda-program-openfpgaloader/`（`OpenFpgaLoaderProgrammer`）：`openFPGALoader -b <board> <bitstream>`；`requiresConfirm=true`；`capabilities: [program/openfpgaloader]`。
- 测试 `tests/plugins/eda_pack_flash_smoke.cpp`：pack 垂直切片（产物 `bitstream`）；flash **未确认→Failed / 已确认→Succeeded**。

### 验收

- `ctest -R eda_` **7/7 Passed**（api/job/toolchain/plugin-host/synth/pnr/pack-flash）
- **完整应用构建** 成功

### 已完成（第二增量）

- `FpgaPackService`（162 行）**移入 `eda-pack-gowin` 并去 wx 化**：插件 `include/GowinPackService.h` + `src/GowinPackService.cpp`（`std::filesystem` + `nlohmann` + `eda_platform` SHA-256/UTC）。
- `main/fpga/FpgaPackService.cpp` 保留为**薄 wx 适配层**（同签名，双向转换请求/报告），`MainFrame` 零改动。
- `GowinPacker::startJob` 接入：运行前 `ValidateInput`，运行后 `Finalize`（记账 sha256/size）+ `WriteManifest`，产物登记写入 `bitstreamSha256`。
- `sigflow` 链接 `eda-pack-gowin` + 插件 include。
- 测试 `eda_pack_flash_smoke` 更新（真实 dummy 可执行 + ≥32B 产物，覆盖 Finalize 记账）。

### 验收

- `ctest -R eda_` **7/7 Passed**
- **完整应用构建** 成功（含 pack 适配层）

### 本增量未做（后续）

- UI 经 `IJobService` 提交打包/烧录（P1-8）；当前 `MainFrame` 仍走 `main/jobs/PackJob`/`FlashJob`。

## P1-4 `eda-sim-verilator`（第一增量完成）

- 契约扩展：`capabilities.h` 新增 `ISimulator`（`sim_build`）与 `SimBuildParams`/`SimBuildResult`。
- 新增插件 `InnerPlugin/eda-sim-verilator/`（`VerilatorSimulator`：`ISimulator` + `IJobProvider`，`jobType=sim.build`）：
  - 经 `IProcessHost` 调 `verilator --binary --trace -Wno-fatal --top-module <top> [tb] <sources> -o <exe>`；
  - 工具发现经 `DefaultToolchain`（`VERILATOR_BIN`/`SIGFLOW_VERILATOR`/PATH）；
  - **去 vcvars 硬编码、去死 DLL**（新插件不含这些）；登记 `sim-executable` 产物。
- 测试 `tests/plugins/eda_sim_verilator_smoke.cpp`：注册表可见 + `sim.build` 垂直切片。

### 验收

- `ctest -R eda_` **8/8 Passed**（api/job/toolchain/plugin-host/synth/pnr/pack-flash/sim-verilator）
- **完整应用构建** 成功

### 本增量未做（后续）

- 无（P1-4 生成器/解析器移入已完成）。

### 已完成（第三增量 / 前端移入）

- 插件 `eda-sim-verilator` 新增 wx-free：`StimulusModel.{h,cpp}`（`StimulusParser`）、`SimTimeline.{h,cpp}`（`TimelineGenerator` + Timeline 类型）、`SimMainCodeGen.{h,cpp}`（`SimMainGenerator`）。
- `main/Simulation/{StimulusParser,TimelineGenerator,SimMainGenerator}.cpp` 保留为**薄 wx 适配层**（`SimModelBridge.h` 转换），`SimulationEngine` 零改动。
- `sigflow` 链接 `eda-sim-verilator`。
- 测试 `tests/plugins/eda_sim_frontend_smoke.cpp`：解析 testbench（DUT/端口/时钟）→ 时间线 → 生成 `sim_main.cpp`。

### 验收

- `ctest -R eda_` **15/15 Passed**；**完整应用构建** 成功。

## P1-5 `eda-target-tangnano9k`（第一增量完成）

- 契约扩展：`include/eda/api/target_profile.hpp`（`eda.target-profile.v1` 的 wx-free `TargetProfile`：id/version/displayName/yosysFamily/device/family/programmerBoard/调试 UART 引脚/波特率）。
- 新增插件 `InnerPlugin/eda-target-tangnano9k/`（`TargetProfileStore`）：`Parse`（nlohmann）/`LoadFile`/`LoadById`，解析仓库 `main/fpga/target-profiles/tang-nano-9k.json`（**取代 `GetTangNano9kTargetProfile` 硬编码**，R7）。
- 测试 `tests/plugins/eda_target_profile_smoke.cpp`：加载真实 profile 文件、`LoadById`、内联 JSON、错误路径。

### 验收

- `ctest -R eda_` **9/9 Passed**
- **完整应用构建** 成功

### 本增量未做（后续）

- 无（P1-5 引脚库 JSON 化已完成）。

### 已完成（第三增量 / 引脚库外提）

- 新增 `main/fpga/target-profiles/tang-nano-9k.pins.json`：88 QFN88 引脚 + 18 板级资源 + 21 IO_TYPE（含 JTAG 56–59 最终保留态）。
- 插件 `eda-target-tangnano9k` 新增 `PinDatabaseStore`（`Parse`/`LoadFile`，wx-free）。
- `main/fpga/FpgaPinData.cpp` **改为从 JSON 加载**（`<exeDir>/target-profiles` 优先，否则仓库上溯；`ApplyReservedPins` 保留兼容），**删除 88 行引脚硬编码表**；`FpgaPinDatabase` wx API 不变。
- 根 CMake：`target-profiles` 目录 POST_BUILD 复制到可执行文件旁。
- 测试 `eda_target_profile_smoke` 增引脚库断言（88/18/21、pin10 可用、pin56 = JTAG TCK）。

### 验收

- `ctest -R eda_` **13/13 Passed**；**完整应用构建** 成功。

### 已完成（第二增量 / 收尾）

- `main/FpgaYosysRuntime.cpp` 的 `GetTangNano9kTargetProfile` / `ResolveFpgaTargetProfile` **改为从 `target-profiles/<id>.json` 解析**（经 `TargetProfileStore`），**删除结构体字面量硬编码**；文件缺失时用内置默认 JSON 兜底。`FpgaToolWindow`/`MainFrame` 调用点零改动。
- `sigflow` 链接 `eda-target-tangnano9k` + 插件 include。

### 验收（收尾）

- **完整应用构建** 成功（profile 解析接入主程序）

## P1-6 `eda-cst-gowin-cst`（第一增量完成）

- 新增插件 `InnerPlugin/eda-cst-gowin-cst/`（`GowinCstCodec`，wx-free）：
  - `Generate(bindings)` → Gowin CST 文本（`IO_LOC "port" pin;` + `IO_PORT "port" IO_TYPE=… [DRIVE=…] [PULL_MODE=UP|DOWN];`）
  - `Parse(text, bindings, errors)` → 按端口名合并 `IO_LOC`/`IO_PORT`；非法行 → error，未知语句 → warning
  - `CstBinding`/`CstError` 结构（`std::string` + `int`）
- 测试 `tests/plugins/eda_gowin_cst_smoke.cpp`：生成、往返、合并、错误/告警路径。

### 验收

- `ctest -R eda_` **10/10 Passed**
- **完整应用构建** 成功

### 本增量未做（后续）

- 无（P1-6 完整移入已完成）。

### 已完成（第二增量 / 完整移入）

- 插件 `eda-cst-gowin-cst` 新增 wx-free：
  - `include/ConstraintSheet.h` + `src/ConstraintSheet.cpp`：`PinBinding`/`ConstraintError`/`ConstraintSheet`/`ConstraintValidator`/`CstGenerator` + `Load/SaveConstraintSheet` + `FindYosysJsonPath`（nlohmann + std::filesystem，校验基于 `eda::target::PinDatabase`）。
  - `include/CstFileValidator.h` + `src/CstFileValidator.cpp`：CST 4 步文件校验（存在→非空→逐行语法→端口覆盖，含 UTF-8 校验）。
- `main/fpga/FpgaConstraint.cpp` / `main/fpga/CstValidator.cpp` 保留为**薄 wx 适配层**（同签名），`FpgaPinBindingPanel`/`NextpnrExecutor`/`FpgaToolWindow` 零改动。
- `sigflow` 链接 `eda-cst-gowin-cst`。
- 测试 `tests/plugins/eda_constraint_sheet_smoke.cpp`：端口解析/校验（向量不完整、保留引脚、重复引脚、未知 IO_TYPE 告警）/生成/导入/文件校验。

### 验收

- `ctest -R eda_` **14/14 Passed**；**完整应用构建** 成功。

## P1-7 声明式工具运行时（第一增量完成）

- 新增核心组件 `core/src/eda-core/DeclarativeTool.{h,cpp}`：
  - `DeclarativeToolSpec`（JSON 描述：`executable` / `arguments` / `working_directory` 支持 `${param}` 占位；`requires_params` / `artifacts` / `requires_confirm` / `capability`）。
  - `DeclarativeJobProvider`（`IJobProvider` + `IPluginInteraction`）：`ParseSpec` → 直接成为一个 Job；`startJob` 做参数校验、占位替换、经 `IProcessHost` 执行、产物登记。
  - **无需为每个工具重编译**：交换 JSON 描述即可切换/新增工具（满足 P1 DoD 的"声明式切换"）。
- 测试 `tests/plugins/eda_declarative_tool_smoke.cpp`：解析、执行、产物、缺参失败。

### 验收

- `ctest -R eda_` **11/11 Passed**
- **完整应用构建** 成功

### 本增量未做（后续）

- YAML 描述（当前用 JSON，仓库无 YAML 依赖）；声明式工具接入 `MainFrame` 工具发现/能力选择（P1-8）。

## P1-9 `eda-wave-vcd`（第一增量完成）

- 契约扩展：`include/eda/api/waveform.hpp`（`IWaveformBackend` + `WaveSignal`/`WaveTransition`/`WaveTimeRange`，wx-free）。
- 新增插件 `InnerPlugin/eda-wave-vcd/`（`VcdWaveformBackend`）：自研 wx-free VCD 解析（`$scope`/`$var`/`$timescale`/值变化），
  **无定容截断**（数据动态增长，取代 `3rd/vcd` 的 32×4096 静默丢弃，R14），资源由 std 容器管理（RAII）。
- 测试 `tests/plugins/eda_wave_vcd_smoke.cpp`：打开/时间刻度/信号元数据/时间范围/Query/ValueAt/未知信号/缺失文件。

### 验收

- `ctest -R eda_` **12/12 Passed**
- **完整应用构建** 成功

### 本增量未做（后续）

- `main/trace/VcdLazyTraceSource` 切到 `IWaveformBackend`；`3rd/vcd` 补 LICENSE 并退役定容实现。

## P1-8 UI 接入（后端管道完成）

- `main/Composer` 现持有 `eda::CoreJobService`（经 `CreatePlatformProcessHost`），并在 `LoadPlugins` 后把内置插件中实现 `IJobProvider` 的实例注册进去；新增 `Composer::JobService()` 与 `Composer::Capabilities()`（能力选择器数据源）。
- 测试：`eda_declarative_switch_smoke` 已证明"能力 → 选择 → 执行"链路。
- **未做**：UI 控件（能力下拉框 / 流程按钮改走 `IJobService`）为 GUI 步骤，需真机运行验证（本环境仅能构建）。

## P1-9 波形数据层（完成接入路径）

- 插件 `eda-wave-vcd`（`VcdWaveformBackend`）：wx-free、无定容、RAII（R14）。
- 新增 `main/trace/WaveformBackendTraceSource.{h,cpp}`：把 `eda::IWaveformBackend` 适配为现有 `sigflow::trace::TraceSource`，为数据层切换提供接入路径。
- 测试 `tests/plugins/eda_wave_trace_adapter_smoke.cpp`。
- **未做**：`3rd/vcd`（无 LICENSE 头、定容静默丢弃）从 `main/debug` 的完全退役（`WaveformAligner`/`Comparator`/`Replay`/`BehaviorSummary` 仍用 `vcd_t*`），属大范围 debug 重构。

## P1 其余任务（剩余）

| 任务 | 状态 |
| --- | --- |
| P1-1…P1-7 | 已完成（P1-7 用 JSON 描述，非 YAML） |
| P1-8 | 后端管道完成；UI 控件未做（GUI） |
| P1-9 | `IWaveformBackend` + 适配器完成；`3rd/vcd` 退役未做 |
| P1-4 | `eda-sim-verilator`（复用 `SimulationEngine` 核心；去 vcvars 硬编码；去死 DLL；补退出码） |
| P1-5 | `eda-target-tangnano9k`（解析 `target-profiles` JSON；删硬编码） |
| P1-6 | `eda-cst-gowin-cst`（`FpgaConstraint` 移入） |
| P1-7 | 声明式工具运行时（YAML → 自动生成 Job） |
| P1-8 | UI：流程按钮接 `IJobService` + 能力下拉选择器 |
| P1-9 | 波形数据层拆 `IWaveformBackend` |
