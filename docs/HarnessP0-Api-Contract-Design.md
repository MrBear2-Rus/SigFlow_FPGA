# Harness P0 最小契约层设计（Approach A）

> 日期：2026-09-21
> 基线仓库：`SigFlow_FPGA_Cmake`（GCC + CMake）
> 上游规划：`docs/HarnessPlan.md` §4（三层接口）、§6（C ABI）、§7（schema）、§8（Job）
> 对应任务：P0-1（契约）+ P0-2/P0-7 骨架；本阶段不落 P0-3~P0-6 的服务实现

---

## 1. 目标与非目标

### 1.1 目标

1. 建立核心与插件之间**唯一编译期契约** `include/eda/api/`（最小子集）。
2. 建立可编译、可链接的最小 `eda_core` 静态库与 `eda_add_plugin()` 构建原语。
3. 提供一个无依赖的契约冒烟测试，作为"契约可编译、可实例化"的证据。
4. 交付 `docs/HarnessP0-ApiCoverage.md`，逐条标注接口"已冻结 / 下轮"，作为后续 P0-3~P0-6 的挂载清单。

### 1.2 非目标（本阶段明确不做）

- 不实现 `PluginHost` / 动态加载 / `eda_plugin_query_v1` 的运行时调用。
- 不建 `core/src/eda-platform`、`core/src/eda-ui`、`InnerPlugin/`、`profiles/`、`bundles/`。
- 不迁移任何现有代码（`main/**` 源集合不动）。
- 不实现 `IJobService` 落地；现有 `main/jobs/*` 不动。
- 不做任何 UI 接入。
- 不改 `sigflow` 可执行目标的编译行为。

---

## 2. 目录与文件

```text
include/eda/api/                 # 新增；唯一公共契约（header-only）
├── Types.h
├── IPluginInteraction.h
├── capabilities.h
├── jobs.hpp
├── abi.h
└── build_info.h

core/
├── CMakeLists.txt               # 新增：eda_core (STATIC)
└── src/eda-core/
    ├── Logger.h
    ├── Logger.cpp
    └── Version.cpp

cmake/EdaPlugin.cmake            # 新增：eda_add_plugin()
tests/contract/
├── CMakeLists.txt
└── eda_api_contract_smoke.cpp

docs/HarnessP0-ApiCoverage.md    # 覆盖清单 + 后续扩展计划
```

顶层 `CMakeLists.txt` 增加 `add_subdirectory(core)` 与 `option(SIGFLOW_BUILD_CONTRACT_TESTS "..." ON)`；`tests/contract` 受该开关控制。

---

## 3. 冻结的接口（最小子集）

### 3.1 `Types.h`

- `enum class ErrorCode { None, NotFound, InvalidArgument, TimedOut, Cancelled, Crashed, Internal, Unsupported };`
- `struct Error { ErrorCode code = ErrorCode::None; std::string message; std::string hint; explicit operator bool() const noexcept; };`
- `template <typename... Args> using Callback = std::function<void(Args...)>;`
- `template <typename T> using EventHandler = std::function<void(const T&)>;`
- `using Json = nlohmann::json;`
- `class Service { public: virtual ~Service() = default; };`（占位，`services.hpp` 落地后替换）
- `struct Artifact { std::string id; std::filesystem::path path; std::string schema; std::string sha256; std::string role; };`
- `struct MethodCall { std::string method; Json params; std::string requestId; };`
- `struct PluginInfo { id/version/displayName/vendor/location/runtime/capabilities/methods/topics/platforms/abi/buildInfo };`
- `toJson` / `fromJson`：以 ADL 为准；契约层仅提供可被特化的声明与 `Json` 直通，未知类型在首次使用处编译报错。

### 3.2 `IPluginInteraction.h`

与 `HarnessPlan.md` §4.1 逐字一致：

```cpp
class IPluginInteraction {
public:
    virtual ~IPluginInteraction() = default;
    virtual void invoke(const MethodCall&, Callback<Error, Json> onReply) = 0;
    virtual std::uint64_t subscribe(const std::string& topic, EventHandler<const Json&>) = 0;
    virtual void unsubscribe(std::uint64_t) = 0;
    virtual PluginInfo info() const = 0;
    virtual void onLoad() {}
    virtual void onUnload() {}
};
```

### 3.3 `capabilities.h`

- 冻结 `EDA_TYPED_METHOD(method, Params, Result)` 宏（HarnessPlan §4.2 语义）。
- 冻结示范能力 `ISynthesizer : public IPluginInteraction`：
  - `virtual std::string synthesizerId() const = 0;`
  - `EDA_TYPED_METHOD(synth_run, SynthParams, SynthResult)`
- 其余能力接口（`ISimulator`/`IPlaceAndRouter`/`IPacker`/`IProgrammer`/`IDesignIR`/`IWaveformBackend`/`ILlmProvider`/`IMcpClient`/`IComponentLibrary`）列入覆盖清单，下轮追加。

### 3.4 `jobs.hpp`

- `enum class JobState { Created, Validating, Queued, Running, ValidatingArtifact, Succeeded, Failed, Cancelled, TimedOut };`
- `struct JobRequest { std::string jobType; std::string pluginId; std::string projectId; Json params; bool requireConfirm = false; };`
- `struct JobTransition { JobState state; std::string timestamp; std::string op; std::string reason; int exitCode = 0; };`
- `struct JobRecord { id/retryOf/request/state/transitions/createdAt/updatedAt/exitCode/traceId/timeoutSec };`
- `class IJobService : public Service { submit/cancel/retry/get/list/report/setConcurrency };`
- `class IJobProvider { jobType/paramsSchema/resultSchema/startJob };`
- `class JobContext { log/progress/cancelled/registerArtifact/emitMetric/jobDir/processHost };`
  - `processHost()` 返回 **forward-declared `IProcessHost&`**（P0-4 定义）。
- 另有 `struct JobReport`（§7.3 / 附录 B.3 的最小可编译子集）。

### 3.5 `abi.h`

- `#define EDA_PLUGIN_ABI_VERSION 1u`
- `struct eda_host_api_v1`、`struct eda_plugin_descriptor_v1`（含 `build_info`）、`extern "C" eda_plugin_query_v1(...)` 声明。
- 本阶段**只冻结结构体布局与签名**，不实现加载。

### 3.6 `build_info.h`

- `EDA_BUILD_INFO` 宏与 `BuildInfo()`：返回编译器 id/版本、C++ 标准、`EDA_PLUGIN_ABI_VERSION`，供 `PluginInfo.buildInfo` / `descriptor.build_info` 构建指纹使用。

---

## 4. 关键决策

1. **JSON = nlohmann**（`include/eda/api/Types.h` 内 `using Json = nlohmann::json`）。理由：与 `HarnessPlan.md` §4.1 明确写法一致，且现有插件侧（`Plugin_DeepSeek`、`CanvasPanel`）已用 nlohmann；`3rd/nlohmann` 已在 include 路径。core 内部 JSON 库维持现状，不在本阶段归一。
2. **`Service` 用最小占位**，避免为最小集引入 `services.hpp`；`services.hpp` 落地时以完整版替换占位并保持 `IJobService` 签名不变。
3. **`IProcessHost` 仅前向声明**，`JobContext::processHost()` 返回其引用但不解引用；P0-4 提供实现。
4. **契约层零行为**：所有头文件不引入 wx / 平台头，只依赖 STL + nlohmann。

---

## 5. 构建接线

- `eda_api`：`add_library(eda_api INTERFACE)`；`target_include_directories(eda_api INTERFACE ${CMAKE_SOURCE_DIR}/include ${SIGFLOW_3RD_DIR})`；`target_compile_features(eda_api INTERFACE cxx_std_20)`。
- `eda_core`：`add_library(eda_core STATIC core/src/eda-core/Logger.cpp core/src/eda-core/Version.cpp)`；`target_link_libraries(eda_core PUBLIC eda_api)`。
- `cmake/EdaPlugin.cmake`：`function(eda_add_plugin NAME)`，本阶段实现 `KIND=STATIC`（`target_link_libraries(${NAME} PUBLIC eda_core)`）；`MODULE`/进程外分支留 TODO 注释，不实现。
- `tests/contract/eda_api_contract_smoke.cpp`：`add_executable(...)` + `target_link_libraries(... eda_core)`；由 `SIGFLOW_BUILD_CONTRACT_TESTS` 控制。

不改动 `sigflow` 的源集合与链接项。

---

## 6. 验收

### 6.1 自动验收

1. `cmake` 配置成功，出现新 target `eda_core`、`eda_api_contract_smoke`。
2. `eda_core` 与 `eda_api_contract_smoke` 编译链接成功（GCC）。
3. `eda_api_contract_smoke` 运行输出 PASS，覆盖：
   - `BuildInfo()` 非空且包含 ABI 版本；
   - `Error` 的 `operator bool`、`ErrorCode` 映射；
   - 编译期断言：`Callback<Error, Json>`、`EventHandler<const Json&>` 可构造；
   - 一个 stub `ISynthesizer` + `IJobProvider` 子类可实例化并实现纯虚接口。
4. 回归：`sigflow` target 仍可被 CMake 正常配置（环境具备 wx 时能构建）。

### 6.2 人工验收

- 人工比对 `include/eda/api/` 与 `HarnessPlan.md` §4.1/§4.2/§6.1/§8.2 的签名一致性。
- 审阅 `docs/HarnessP0-ApiCoverage.md` 的"已冻结 / 下轮"划分是否合理。

---

## 7. 风险与对策

| 风险 | 对策 |
| --- | --- |
| `Service` 占位与后续 `services.hpp` 冲突 | 占位最小、签名稳定；替换时只改基类定义，不改 `IJobService` 成员 |
| `toJson/fromJson` 泛型在最小集下误用 | 只对 `ISynthesizer` 的 Params/Result 提供特化样例；未知类型编译期报错 |
| nlohmann 版本与插件侧不一致 | 复用仓库同一份 `3rd/nlohmann/json.hpp`，无新增依赖 |
| 顶层 CMake 改动影响 `sigflow` | 新 target 独立；不改 `sigflow` 源/链接；`SIGFLOW_BUILD_CONTRACT_TESTS` 可关 |

---

## 8. 覆盖清单（摘要，详见 `docs/HarnessP0-ApiCoverage.md`）

- 本轮冻结：`Types`（Error/Callback/Json/Service/Artifact/MethodCall/PluginInfo）、`IPluginInteraction`、`EDA_TYPED_METHOD`、`ISynthesizer`、`jobs`（9 态 + JobRequest/Record/Context + IJobService/IJobProvider）、`abi` 结构体、`build_info`。
- 下轮：其余能力接口、`services.hpp`/`events.hpp`/`schemas.hpp`、`text_document.hpp`、`PluginHost`、`IProcessHost`、`IToolchain`、`IProject`、动态加载。
