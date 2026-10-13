# Harness P0 最小契约层 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在 `SigFlow_FPGA_Cmake` 建立 core↔plugin 的唯一编译期契约 `include/eda/api/`（最小子集）+ 可编译的 `eda_core` 静态库 + `eda_add_plugin()` 构建原语 + 契约冒烟测试，且不改动 `sigflow` 目标的任何行为。

**Architecture:** header-only 契约层（INTERFACE target `eda_api`）依赖仅 STL + `3rd/nlohmann`；`eda_core`（STATIC）提供 `Logger` 与 `BuildInfo`；`cmake/EdaPlugin.cmake` 提供 `eda_add_plugin()`（本阶段只实现 `KIND=STATIC`）。契约不引入 wx/平台头，`sigflow` 源集合与链接项完全不动。

**Tech Stack:** C++20、CMake ≥3.21、GCC（Windows=MinGW-w64 g++ 12.2.0，Linux=GCC）、nlohmann/json、CTest。

**基线：** 仓库 `E:\EDA_Race\Cangku\new\SigFlow_FPGA_Cmake`，生成器 `MinGW Makefiles`，`CMAKE_CXX_COMPILER=E:/download/x86_64-12.2.0-release-posix-seh-rt_v10-rev0/mingw64/bin/g++.exe`，`make=.../mingw64/bin/make.exe`。

**参考：** 设计文档 `docs/HarnessP0-Api-Contract-Design.md`；上游 `docs/HarnessPlan.md` §4/§6/§7/§8/§12。

---

## File Structure

| 文件 | 职责 |
| --- | --- |
| `include/eda/api/Types.h` | 公共类型：Error/ErrorCode/ToString、Callback、EventHandler、Json、Service、Artifact、MethodCall、PluginInfo、fromJson 主模板 |
| `include/eda/api/IPluginInteraction.h` | 第一层接口：invoke/subscribe/unsubscribe/info/onLoad/onUnload |
| `include/eda/api/capabilities.h` | `EDA_TYPED_METHOD` 宏 + 示范能力 `ISynthesizer` + SynthParams/SynthResult 转换 |
| `include/eda/api/jobs.hpp` | 9 态 JobState、Request/Transition/Record/Report、JobContext、IJobService、IJobProvider |
| `include/eda/api/abi.h` | `EDA_PLUGIN_ABI_VERSION`、`eda_host_api_v1`、`eda_plugin_descriptor_v1`、`eda_plugin_query_v1` |
| `include/eda/api/build_info.h` | `BuildInfo()` 声明 + `EDA_BUILD_INFO_ABI` |
| `core/src/eda-core/Logger.h/.cpp` | 进程内极简日志（可替换 sink） |
| `core/src/eda-core/Version.cpp` | `BuildInfo()` 实现（编译器/标准/ABI） |
| `core/CMakeLists.txt` | `eda_core` STATIC target |
| `cmake/EdaPlugin.cmake` | `eda_add_plugin()` |
| `tests/contract/eda_api_contract_smoke.cpp` | 契约冒烟：类型/错误码/多态 stub/typed method |
| `tests/contract/CMakeLists.txt` | 冒烟 target + `add_test` |
| `docs/HarnessP0-ApiCoverage.md` | 接口覆盖清单（已冻结 / 下轮） |
| `CMakeLists.txt`（改） | 定义 `eda_api`、`add_subdirectory(core)`、`option(SIGFLOW_BUILD_CONTRACT_TESTS)`、`include(cmake/EdaPlugin.cmake)` |

---

## Task 1: 契约基础类型 `Types.h`

**Files:**
- Create: `include/eda/api/Types.h`
- Test: `tests/contract/eda_api_contract_smoke.cpp`（本任务先生成"只测 Types"版本）

- [ ] **Step 1: 写失败测试**（`tests/contract/eda_api_contract_smoke.cpp`）

```cpp
#include <eda/api/Types.h>

#include <iostream>

namespace {
int g_failures = 0;
void Check(bool ok, const char* msg) {
    if (ok) { std::cout << "  ok: " << msg << "\n"; }
    else { ++g_failures; std::cout << "  FAIL: " << msg << "\n"; }
}
} // namespace

int main() {
    eda::Error ok;
    Check(!static_cast<bool>(ok), "default Error is false");
    Check(eda::Error::Ok().code == eda::ErrorCode::None, "Error::Ok code None");

    eda::Error bad{eda::ErrorCode::TimedOut, "t", ""};
    Check(static_cast<bool>(bad), "non-None Error is true");
    Check(std::string(eda::ToString(bad.code)) == "TimedOut", "ToString(TimedOut)");

    eda::MethodCall call;
    call.method = "synth.run";
    call.params = eda::Json{{"x", 1}};
    Check(call.params["x"] == 1, "MethodCall carries Json params");

    eda::PluginInfo info;
    info.id = "stub";
    info.abi = 1;
    Check(info.id == "stub" && info.abi == 1, "PluginInfo fields");

    std::cout << (g_failures == 0 ? "ALL PASS" : "FAILURES") << "\n";
    return g_failures == 0 ? 0 : 1;
}
```

- [ ] **Step 2: 运行确认失败**

Run（直接从源码编译，避免依赖根 CMake 尚未接线；Git Bash / PowerShell 均可）:
```
& "E:/download/x86_64-12.2.0-release-posix-seh-rt_v10-rev0/mingw64/bin/g++.exe" -std=c++20 -I"include" -I"3rd" "tests/contract/eda_api_contract_smoke.cpp" -o "$env:TEMP/eda_smoke.exe"
```
Expected: FAIL — `fatal error: eda/api/Types.h: No such file or directory`。

- [ ] **Step 3: 实现 `include/eda/api/Types.h`**

```cpp
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace eda {

using Json = nlohmann::json;

enum class ErrorCode {
    None,
    NotFound,
    InvalidArgument,
    TimedOut,
    Cancelled,
    Crashed,
    Internal,
    Unsupported,
};

inline const char* ToString(ErrorCode code) noexcept {
    switch (code) {
        case ErrorCode::None:           return "None";
        case ErrorCode::NotFound:       return "NotFound";
        case ErrorCode::InvalidArgument:return "InvalidArgument";
        case ErrorCode::TimedOut:       return "TimedOut";
        case ErrorCode::Cancelled:      return "Cancelled";
        case ErrorCode::Crashed:        return "Crashed";
        case ErrorCode::Internal:       return "Internal";
        case ErrorCode::Unsupported:    return "Unsupported";
    }
    return "Unknown";
}

struct Error {
    ErrorCode code = ErrorCode::None;
    std::string message;
    std::string hint;

    explicit operator bool() const noexcept { return code != ErrorCode::None; }
    static Error Ok() { return {}; }
};

template <typename... Args>
using Callback = std::function<void(Args...)>;

template <typename T>
using EventHandler = std::function<void(const T&)>;

class Service {
public:
    virtual ~Service() = default;
};

struct Artifact {
    std::string id;
    std::filesystem::path path;
    std::string schema;
    std::string sha256;
    std::string role;
};

struct MethodCall {
    std::string method;
    Json params;
    std::string requestId;
};

struct PluginInfo {
    std::string id;
    std::string version;
    std::string displayName;
    std::string vendor;
    std::string location;   // "inner" | "external"
    std::string runtime;    // "inprocess" | "process"
    std::vector<std::string> capabilities;
    std::vector<std::string> methods;
    std::vector<std::string> topics;
    std::vector<std::string> platforms;
    std::uint32_t abi = 0;
    std::string buildInfo;
};

inline Json toJson(const Json& value) { return value; }

template <typename T>
T fromJson(const Json& value);

} // namespace eda
```

- [ ] **Step 4: 运行确认通过**

Run: 同 Step 2 的 g++ 命令，然后 `& "$env:TEMP/eda_smoke.exe"`
Expected: 输出 `ALL PASS`，退出码 0。

- [ ] **Step 5: Commit（仅在获得授权时执行）**

```bash
git add include/eda/api/Types.h tests/contract/eda_api_contract_smoke.cpp
git commit -m "feat(eda-api): add contract base types"
```

---

## Task 2: 第一层接口 + 示范能力 `IPluginInteraction.h` / `capabilities.h`

**Files:**
- Create: `include/eda/api/IPluginInteraction.h`
- Create: `include/eda/api/capabilities.h`
- Test: `tests/contract/eda_api_contract_smoke.cpp`（追加 stub 与 typed-method 断言）

- [ ] **Step 1: 追加失败测试**

在 `eda_api_contract_smoke.cpp` 顶部追加 include，并在 `main` 内追加：

```cpp
#include <eda/api/IPluginInteraction.h>
#include <eda/api/capabilities.h>

// ... 在 main() 中追加：
    class StubSynth final : public eda::ISynthesizer {
    public:
        std::string synthesizerId() const override { return "stub"; }
        void invoke(const eda::MethodCall&, eda::Callback<eda::Error, eda::Json> cb) override {
            cb(eda::Error{}, eda::Json{{"job_id", "J1"}});
        }
        std::uint64_t subscribe(const std::string&, eda::EventHandler<const eda::Json&>) override { return 1; }
        void unsubscribe(std::uint64_t) override {}
        eda::PluginInfo info() const override { eda::PluginInfo i; i.id = "stub"; return i; }
    };

    StubSynth synth;
    Check(synth.synthesizerId() == "stub", "ISynthesizer::synthesizerId");

    bool dispatched = false;
    synth.synth_run(eda::SynthParams{"top", {"top.v"}, "baseline"},
                    [&](eda::Error e, eda::SynthResult r) {
                        dispatched = !e && r.jobId == "J1";
                    });
    Check(dispatched, "EDA_TYPED_METHOD dispatch + fromJson");
```

- [ ] **Step 2: 运行确认失败**

Run: g++ 编译命令同上。Expected: FAIL — 找不到 `eda/api/IPluginInteraction.h`。

- [ ] **Step 3: 实现 `IPluginInteraction.h`**

```cpp
#pragma once

#include <cstdint>
#include <string>

#include "Types.h"

namespace eda {

class IPluginInteraction {
public:
    virtual ~IPluginInteraction() = default;

    virtual void invoke(const MethodCall& call, Callback<Error, Json> onReply) = 0;
    virtual std::uint64_t subscribe(const std::string& topic, EventHandler<const Json&> handler) = 0;
    virtual void unsubscribe(std::uint64_t subscriptionId) = 0;
    virtual PluginInfo info() const = 0;

    virtual void onLoad() {}
    virtual void onUnload() {}
};

} // namespace eda
```

- [ ] **Step 4: 实现 `capabilities.h`**

```cpp
#pragma once

#include <string>
#include <utility>
#include <vector>

#include "IPluginInteraction.h"

namespace eda {

#define EDA_TYPED_METHOD(method, MethodParams, MethodResult)                          \
    virtual void method(const MethodParams& p, Callback<Error, MethodResult> cb) {    \
        invoke(MethodCall{#method, toJson(p), {}},                                    \
               [cb = std::move(cb)](Error e, Json r) {                                \
                   if (static_cast<bool>(e)) { cb(e, MethodResult{}); return; }       \
                   cb(Error::Ok(), fromJson<MethodResult>(r));                        \
               });                                                                    \
    }                                                                                 \
    static constexpr const char* k##method = #method

struct SynthParams {
    std::string topModule;
    std::vector<std::string> sourceFiles;
    std::string strategy = "baseline";
};

struct SynthResult {
    std::string jobId;
};

inline Json toJson(const SynthParams& p) {
    return Json{{"top_module", p.topModule},
                {"source_files", p.sourceFiles},
                {"strategy", p.strategy}};
}

inline Json toJson(const SynthResult& r) {
    return Json{{"job_id", r.jobId}};
}

template <>
inline SynthResult fromJson<SynthResult>(const Json& j) {
    return SynthResult{j.value("job_id", std::string{})};
}

class ISynthesizer : public IPluginInteraction {
public:
    virtual std::string synthesizerId() const = 0;
    EDA_TYPED_METHOD(synth_run, SynthParams, SynthResult);
};

} // namespace eda
```

- [ ] **Step 5: 运行确认通过**

Run: g++ 编译 + 运行。Expected: `ALL PASS`。

- [ ] **Step 6: Commit（仅在获得授权时执行）**

```bash
git add include/eda/api/IPluginInteraction.h include/eda/api/capabilities.h tests/contract/eda_api_contract_smoke.cpp
git commit -m "feat(eda-api): add plugin interaction interface and typed-method macro"
```

---

## Task 3: Job 与 ABI 契约 `jobs.hpp` / `abi.h` / `build_info.h`

**Files:**
- Create: `include/eda/api/jobs.hpp`
- Create: `include/eda/api/abi.h`
- Create: `include/eda/api/build_info.h`
- Test: `tests/contract/eda_api_contract_smoke.cpp`（追加 Job/ABI 断言）

- [ ] **Step 1: 追加失败测试**

```cpp
#include <eda/api/jobs.hpp>
#include <eda/api/abi.h>

// ... 在 main() 中追加：
    eda::JobRequest req;
    req.jobType = "synth";
    req.params = eda::Json::object();
    Check(req.jobType == "synth", "JobRequest fields");

    eda::JobRecord rec;
    rec.id = "J1";
    rec.state = eda::JobState::Queued;
    Check(rec.state == eda::JobState::Queued, "JobState enum");

    class StubProvider final : public eda::IJobProvider {
    public:
        std::string jobType() const override { return "synth"; }
        eda::Json paramsSchema() const override { return eda::Json::object(); }
        eda::Json resultSchema() const override { return eda::Json::object(); }
        void startJob(const eda::JobRequest&, eda::JobContext&) override {}
    };
    StubProvider provider;
    Check(provider.jobType() == "synth", "IJobProvider stub compiles");

    Check(EDA_PLUGIN_ABI_VERSION == 1u, "ABI version macro");
```

- [ ] **Step 2: 运行确认失败**

Run: g++ 编译。Expected: FAIL — 找不到 `jobs.hpp` / `abi.h`。

- [ ] **Step 3: 实现 `jobs.hpp`**

```cpp
#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "Types.h"

namespace eda {

class IProcessHost; // P0-4 定义；此处仅前向声明

enum class JobState {
    Created,
    Validating,
    Queued,
    Running,
    ValidatingArtifact,
    Succeeded,
    Failed,
    Cancelled,
    TimedOut,
};

struct JobRequest {
    std::string jobType;
    std::string pluginId;
    std::string projectId;
    Json params;
    bool requireConfirm = false;
};

struct JobTransition {
    JobState state = JobState::Created;
    std::string timestamp;
    std::string op;
    std::string reason;
    int exitCode = 0;
};

struct JobRecord {
    std::string id;
    std::string retryOf;
    JobRequest request;
    JobState state = JobState::Created;
    std::vector<JobTransition> transitions;
    std::string createdAt;
    std::string updatedAt;
    int exitCode = 0;
    std::string traceId;
    std::optional<int> timeoutSec;
};

struct JobReport {
    std::string schemaVersion = "eda.jobreport.v1";
    std::string jobId;
    std::string jobType;
    std::string pluginId;
    JobState state = JobState::Created;
    int exitCode = 0;
    std::vector<Artifact> artifacts;
    Json metrics;
    Json diagnostics;
};

class JobContext {
public:
    virtual ~JobContext() = default;
    virtual void log(const std::string& line, bool isError) = 0;
    virtual void progress(int percent, const std::string& status) = 0;
    virtual bool cancelled() const = 0;
    virtual void registerArtifact(const Artifact& artifact) = 0;
    virtual void emitMetric(const Json& metric) = 0;
    virtual std::filesystem::path jobDir() const = 0;
    virtual IProcessHost& processHost() = 0;
};

class IJobProvider {
public:
    virtual ~IJobProvider() = default;
    virtual std::string jobType() const = 0;
    virtual Json paramsSchema() const = 0;
    virtual Json resultSchema() const = 0;
    virtual void startJob(const JobRequest& request, JobContext& ctx) = 0;
};

class IJobService : public Service {
public:
    virtual std::string submit(const JobRequest& request) = 0;
    virtual bool cancel(const std::string& jobId, const std::string& reason) = 0;
    virtual bool retry(const std::string& jobId, std::string& outNewId) = 0;
    virtual std::optional<JobRecord> get(const std::string& jobId) = 0;
    virtual std::vector<JobRecord> list(const std::string& projectId) = 0;
    virtual JobReport report(const std::string& jobId) = 0;
    virtual void setConcurrency(std::size_t n) = 0;
};

} // namespace eda
```

- [ ] **Step 4: 实现 `abi.h`**

```cpp
#pragma once

#include <cstdint>

#ifndef EDA_API
#define EDA_API
#endif

#define EDA_PLUGIN_ABI_VERSION 1u

extern "C" {

struct eda_host_api_v1; // opaque，P0-3/P0-4 定义实现

typedef struct eda_plugin_descriptor_v1 {
    std::uint32_t struct_size;
    std::uint32_t abi_version;
    const char* id;
    const char* version;
    const char* manifest_json;
    const char* build_info;
    void (*register_plugin)(struct eda_host_api_v1* host, void** out_instance);
    void (*unregister_plugin)(void* instance);
} eda_plugin_descriptor_v1;

EDA_API const eda_plugin_descriptor_v1* eda_plugin_query_v1(std::uint32_t host_abi);

} // extern "C"
```

- [ ] **Step 5: 实现 `build_info.h`**

```cpp
#pragma once

#include <string>

#include "abi.h"

namespace eda {

// 返回编译器 id/版本、C++ 标准与 ABI 版本，供 PluginInfo.buildInfo /
// descriptor.build_info 的构建指纹比对使用。
std::string BuildInfo();

} // namespace eda

#define EDA_BUILD_INFO_ABI EDA_PLUGIN_ABI_VERSION
```

- [ ] **Step 6: 运行确认通过**

Run: g++ 编译 + 运行。Expected: `ALL PASS`。

- [ ] **Step 7: Commit（仅在获得授权时执行）**

```bash
git add include/eda/api/jobs.hpp include/eda/api/abi.h include/eda/api/build_info.h tests/contract/eda_api_contract_smoke.cpp
git commit -m "feat(eda-api): add job and ABI contracts"
```

---

## Task 4: `eda_core` 静态库 + CMake 接线

**Files:**
- Create: `core/src/eda-core/Logger.h`
- Create: `core/src/eda-core/Logger.cpp`
- Create: `core/src/eda-core/Version.cpp`
- Create: `core/CMakeLists.txt`
- Create: `cmake/EdaPlugin.cmake`
- Modify: `CMakeLists.txt`（在 `add_executable(sigflow ...)` 之前插入契约接线）
- Test: 追加 `BuildInfo()` 断言；`tests/contract/CMakeLists.txt`

- [ ] **Step 1: 追加失败测试**

```cpp
#include <eda/api/build_info.h>

// ... 在 main() 中追加：
    Check(!eda::BuildInfo().empty(), "BuildInfo() non-empty");
```

- [ ] **Step 2: 运行确认失败**

Run: g++ 编译（不带 Version.cpp）。Expected: LINK 失败 — `undefined reference to eda::BuildInfo()`。

- [ ] **Step 3: 实现 `Logger.h` / `Logger.cpp`**

`core/src/eda-core/Logger.h`
```cpp
#pragma once

#include <functional>
#include <string>

namespace eda {

enum class LogLevel { Debug, Info, Warning, Error };

using LogSink = std::function<void(LogLevel, const std::string&)>;

// 进程内日志下沉点；未设置时静默丢弃。宿主/测试可注入自己的 sink。
void SetLogSink(LogSink sink);
void Log(LogLevel level, const std::string& message);

} // namespace eda
```

`core/src/eda-core/Logger.cpp`
```cpp
#include "Logger.h"

#include <utility>

namespace eda {
namespace {
LogSink& Sink() {
    static LogSink sink;
    return sink;
}
} // namespace

void SetLogSink(LogSink sink) { Sink() = std::move(sink); }

void Log(LogLevel level, const std::string& message) {
    if (Sink()) Sink()(level, message);
}

} // namespace eda
```

- [ ] **Step 4: 实现 `Version.cpp`**

```cpp
#include <eda/api/build_info.h>

#include <sstream>

namespace eda {

std::string BuildInfo() {
    std::ostringstream os;
    os << "compiler=";
#if defined(__GNUC__)
    os << "gcc-" << __GNUC__ << '.' << __GNUC_MINOR__ << '.' << __GNUC_PATCHLEVEL__;
#elif defined(_MSC_VER)
    os << "msvc-" << _MSC_VER;
#else
    os << "unknown";
#endif
    os << ";cxx=" << __cplusplus << ";abi=" << EDA_PLUGIN_ABI_VERSION;
    return os.str();
}

} // namespace eda
```

- [ ] **Step 5: 实现 `core/CMakeLists.txt`**

```cmake
add_library(eda_core STATIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src/eda-core/Logger.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/eda-core/Version.cpp"
)

target_link_libraries(eda_core PUBLIC eda_api)
```

- [ ] **Step 6: 实现 `cmake/EdaPlugin.cmake`**

```cmake
include_guard(GLOBAL)

# eda_add_plugin(<name> SOURCES <files...> [KIND STATIC])
#
# P0 仅实现 KIND=STATIC（官方插件编译进核心，零 ABI 风险）。
# MODULE / 进程外分支在 P0-3/P3 落地，未实现前显式报错，避免静默降级。
function(eda_add_plugin NAME)
    cmake_parse_arguments(EDA_P "" "KIND" "SOURCES" ${ARGN})
    if(NOT EDA_P_KIND)
        set(EDA_P_KIND STATIC)
    endif()
    if(NOT EDA_P_SOURCES)
        message(FATAL_ERROR "eda_add_plugin(${NAME}): SOURCES is required")
    endif()
    if(EDA_P_KIND STREQUAL "STATIC")
        add_library(${NAME} STATIC ${EDA_P_SOURCES})
        target_link_libraries(${NAME} PUBLIC eda_core)
        return()
    endif()
    message(FATAL_ERROR
        "eda_add_plugin(${NAME}): KIND '${EDA_P_KIND}' not implemented in P0 (only STATIC)")
endfunction()
```

- [ ] **Step 7: 修改根 `CMakeLists.txt`**

在 `add_executable(sigflow WIN32 ...)` 之前插入：

```cmake
# ---------------------------------------------------------------------------
# Harness P0：核心↔插件编译期契约（不参与 sigflow 目标的源集合）
# ---------------------------------------------------------------------------
add_library(eda_api INTERFACE)
target_include_directories(eda_api INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${SIGFLOW_3RD_DIR}"
)
target_compile_features(eda_api INTERFACE cxx_std_20)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/EdaPlugin.cmake")

add_subdirectory(core)

option(SIGFLOW_BUILD_CONTRACT_TESTS "Build the eda API contract smoke test" ON)
if(SIGFLOW_BUILD_CONTRACT_TESTS)
    enable_testing()
    add_subdirectory(tests/contract)
endif()
```

- [ ] **Step 8: 实现 `tests/contract/CMakeLists.txt`**

```cmake
add_executable(eda_api_contract_smoke eda_api_contract_smoke.cpp)
target_link_libraries(eda_api_contract_smoke PRIVATE eda_core)

add_test(NAME eda_api_contract_smoke COMMAND eda_api_contract_smoke)
```

- [ ] **Step 9: 构建并运行（新 build 目录，不污染现有 build-gcc）**

```
cmake -S . -B build-contract -G "MinGW Makefiles" ^
  -DCMAKE_CXX_COMPILER=E:/download/x86_64-12.2.0-release-posix-seh-rt_v10-rev0/mingw64/bin/g++.exe ^
  -DCMAKE_MAKE_PROGRAM=E:/download/x86_64-12.2.0-release-posix-seh-rt_v10-rev0/mingw64/bin/make.exe ^
  -DSIGFLOW_BUILD_CONTRACT_TESTS=ON
cmake --build build-contract --target eda_api_contract_smoke
ctest --test-dir build-contract -R eda_api_contract_smoke --output-on-failure
```
Expected: 构建成功，CTest PASS。若根配置因 wx/slang 缺失失败，退回 Step 2 的直编方式验证：
```
& "E:/download/.../mingw64/bin/g++.exe" -std=c++20 -Iinclude -I3rd `
  tests/contract/eda_api_contract_smoke.cpp core/src/eda-core/Logger.cpp core/src/eda-core/Version.cpp `
  -o "$env:TEMP/eda_smoke.exe"; & "$env:TEMP/eda_smoke.exe"
```

- [ ] **Step 10: Commit（仅在获得授权时执行）**

```bash
git add core/ cmake/EdaPlugin.cmake CMakeLists.txt tests/contract/
git commit -m "feat(eda-core): add core lib, EdaPlugin.cmake and contract smoke"
```

---

## Task 5: 覆盖清单与验收

**Files:**
- Create: `docs/HarnessP0-ApiCoverage.md`

- [ ] **Step 1: 编写覆盖清单**（逐条列 `HarnessPlan.md` §4.1/§4.2/§6.1/§8.2，标注"已冻结 / 下轮"）

必须覆盖以下条目并给出结论：
- §4.1 `IPluginInteraction`、`MethodCall`、`PluginInfo` → 已冻结
- §4.2 `IHDLFrontend/ISimulator/ISynthesizer/IPlaceAndRouter/IPacker/IProgrammer/IDesignIR/IWaveformBackend/ILlmProvider/IMcpClient/IComponentLibrary` → 仅 `ISynthesizer` 已冻结，其余下轮
- §6.1 `eda_plugin_descriptor_v1/eda_host_api_v1/eda_plugin_query_v1/EDA_PLUGIN_ABI_VERSION` → 已冻结（仅结构体，加载实现下轮）
- §7.3 冻结 schema 列表（plugin-manifest/job/jobreport/target-profile/ir/netlist）→ 全部下轮
- §8.2 `JobState/JobRequest/JobRecord/JobTransition/IJobService/IJobProvider/JobContext` → 已冻结；`JobContext::processHost` 前向声明，`IProcessHost` 下轮
- §8.3 状态迁移表 → 下轮（P0-5）
- `services.hpp/events.hpp/schemas.hpp/text_document.hpp` → 下轮（P0-2/P0-3）

- [ ] **Step 2: 最终验收（全部一次性跑通）**

```
cmake --build build-contract --target eda_api_contract_smoke
ctest --test-dir build-contract -R eda_api_contract_smoke --output-on-failure
```
Expected: PASS。另确认未改动 `sigflow`：`git status` 中 `sigflow` 源集合无新增/删除（仅有本计划列出的文件变化）。

- [ ] **Step 3: Commit（仅在获得授权时执行）**

```bash
git add docs/HarnessP0-ApiCoverage.md
git commit -m "docs: add Harness P0 API coverage checklist"
```

---

## Self-Review

**Spec coverage**
- 设计 §3.1 Types → Task 1 ✔
- 设计 §3.2 IPluginInteraction → Task 2 ✔
- 设计 §3.3 capabilities/ISynthesizer → Task 2 ✔
- 设计 §3.4 jobs → Task 3 ✔
- 设计 §3.5 abi → Task 3 ✔
- 设计 §3.6 build_info → Task 3/4 ✔
- 设计 §5 构建接线 → Task 4 ✔
- 设计 §6 验收 → Task 4 Step 9 + Task 5 ✔
- 设计 §8 覆盖清单 → Task 5 ✔

**Placeholder scan:** 无 TBD/TODO；`cmake/EdaPlugin.cmake` 的 `FATAL_ERROR` 是显式未实现分支，非占位。

**Type consistency:** `Error::Ok()`、`ToString(ErrorCode)`、`fromJson<T>` 主模板、`EDA_TYPED_METHOD`、`JobState` 九态、`IProcessHost` 前向声明在 Task 1–4 间一致；`SynthParams/SynthResult` 在 Task 2 定义并在测试中按相同字段使用。

**Commands:** 编译器/生成器路径来自 `build-gcc/CMakeCache.txt` 实测；新 build 目录 `build-contract` 避免污染现有 `build-gcc`。
