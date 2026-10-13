// NG-10（前半）：运行期 schema 校验 + 边界/限额矩阵（真实 loopback HTTP）。
//
// 本测试启动真实的 eda::agent::GatewayServer，用 httplib 客户端打真实路由，然后：
//   A. 把每个真实响应的 data 载荷与 contracts/edu-agent/v1/schemas/*.schema.json 对齐校验。
//      注册表已支持 `$ref`（文件引用 + 内部 JSON Pointer）且 required/enum/const 在**每一层**
//      对象上生效，因此嵌套结构（context-query.design_nodes、job-report-view.diagnostics 等）
//      同样被校验；`additionalProperties` 仍不强制（契约要求忽略未知可选字段）。
//       集合元素另做逐条校验（每个 event、每条 legacy job），便于定位到具体下标；
//   B. 边界/限额矩阵：1 MiB 请求体、2 MiB 响应护栏、集合分页夹紧（100/500、256/4096）、
//      64 位十进制字符串、中文+空格相对路径、并发只读访问、事件游标过期、未知路径/方法兜底；
//   C. 结尾打印本次运行**实测**到的限额与平台串（不打印未观测的数字）。
//
// 判定只依赖对外可观察行为（状态码 / 错误码 / 载荷字段 / schema 校验结果），不依赖内部实现。
//
// 历史缺陷（本轮已修，测试保持断言契约形状）：非 ASCII 相对源路径曾无法加载、
// `/sources/{id}` 曾对 UTF-8 路径返回 410；生产侧已改用 Platform::PathFromUtf8。
// `GET /waves/{a}/signals` 的 time_range 也曾与 schema 不一致，已对齐为
// {start_tick,end_tick} 十进制字符串。
#include "eda-agent-gateway/GatewayServer.h"

#include <eda/api/Types.h>
#include <eda/api/jobs.hpp>
#include <eda/api/waveform.hpp>

#include "eda-core/SchemaRegistry.h"
#include "eda-platform/Platform.h"
#include "eda-platform/Sha256.h"

#include <httplib.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {

int g_failures = 0;

void Check(bool ok, const char* msg) {
    if (ok) {
        std::cout << "  ok: " << msg << "\n";
    } else {
        ++g_failures;
        std::cout << "  FAIL: " << msg << "\n";
    }
}

// 便于拼接带上下文的断言说明（如 "GET /projects/p/state"）。
void Check(bool ok, const std::string& msg) { Check(ok, msg.c_str()); }

// ---------------------------------------------------------------------------
// 载荷访问助手：失败信封没有 data 时也不会触发 nlohmann 的 const operator[] 断言。
// ---------------------------------------------------------------------------
const eda::Json& NullJson() {
    static const eda::Json kNull;
    return kNull;
}

const eda::Json& At(const eda::Json& node, const char* key) {
    if (!node.is_object()) return NullJson();
    const auto it = node.find(key);
    return it == node.end() ? NullJson() : *it;
}

const eda::Json& Item(const eda::Json& node, std::size_t index) {
    return node.is_array() && index < node.size() ? node[index] : NullJson();
}

const eda::Json& DataOf(const eda::Json& body) { return At(body, "data"); }

std::string Str(const eda::Json& node, const char* key) {
    const eda::Json& value = At(node, key);
    return value.is_string() ? value.get<std::string>() : std::string();
}

bool Bool(const eda::Json& node, const char* key, bool fallback) {
    const eda::Json& value = At(node, key);
    return value.is_boolean() ? value.get<bool>() : fallback;
}

long long Int(const eda::Json& node, const char* key, long long fallback) {
    const eda::Json& value = At(node, key);
    if (value.is_number_integer() || value.is_number_unsigned()) return value.get<long long>();
    return fallback;
}

std::size_t Count(const eda::Json& node) { return node.is_array() ? node.size() : 0; }

bool IsDecimalString(const eda::Json& value) {
    if (!value.is_string()) return false;
    const std::string text = value.get<std::string>();
    return !text.empty() && std::all_of(text.begin(), text.end(), [](unsigned char c) {
               return c >= '0' && c <= '9';
           });
}

std::string ErrorCode(const eda::Json& body) { return Str(At(body, "error"), "code"); }

// 成功/失败信封的公共三件套（openapi components.schemas.Envelope）。
bool EnvelopeOk(const eda::Json& body) {
    return body.is_object() && Str(body, "schema_version") == "edu.api.v1" &&
           !Str(body, "request_id").empty() && !Str(body, "trace_id").empty();
}

// ---------------------------------------------------------------------------
// 假服务：与 edu_gateway_smoke.cpp 同款，但加锁——并发用例会从多个 HTTP worker
// 线程同时读它。
// ---------------------------------------------------------------------------
class FakeJobService final : public eda::IJobService {
public:
    std::string submit(const eda::JobRequest& request) override {
        const std::string id = "job-" + std::to_string(++sequence_);
        std::lock_guard<std::mutex> lock(mutex_);
        records_[id].id = id;
        records_[id].request = request;
        records_[id].state = eda::JobState::Queued;
        return id;
    }
    bool cancel(const std::string& jobId, const std::string&) override {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = records_.find(jobId);
        if (it == records_.end()) return false;
        it->second.state = eda::JobState::Cancelled;
        return true;
    }
    bool retry(const std::string&, std::string&) override { return false; }
    std::optional<eda::JobRecord> get(const std::string& jobId) override {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = records_.find(jobId);
        if (it == records_.end()) return std::nullopt;
        return it->second;
    }
    std::vector<eda::JobRecord> list(const std::string&) override { return {}; }
    eda::JobReport report(const std::string& jobId) override {
        eda::JobReport report;
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = records_.find(jobId);
        if (it == records_.end()) return report;
        report.jobId = jobId;
        report.jobType = it->second.request.jobType;
        report.pluginId = "fake";
        report.state = it->second.state;
        return report;
    }
    void setConcurrency(std::size_t) override {}

private:
    mutable std::mutex mutex_;
    std::atomic<int> sequence_{0};
    std::map<std::string, eda::JobRecord> records_;
};

// 多信号假波形后端：验证信号列表 256/4096 分页夹紧与 64 位 tick 边界。
class WideWaveBackend final : public eda::IWaveformBackend {
public:
    explicit WideWaveBackend(int signalCount) : signalCount_(signalCount) {}

    bool Open(const std::string& path, std::string& error) override {
        std::ifstream probe(path, std::ios::binary);
        if (!probe) {
            error = "cannot open waveform file";
            return false;
        }
        signals_.clear();
        signals_.reserve(static_cast<std::size_t>(signalCount_));
        for (int i = 0; i < signalCount_; ++i) {
            eda::WaveSignal signal;
            signal.id = i;
            signal.name = "sig" + std::to_string(i);
            signal.scope = "TOP";
            signal.fullName = "TOP.sig" + std::to_string(i);
            signal.idCode = "!" + std::to_string(i);
            signal.width = 1;
            signals_.push_back(std::move(signal));
        }
        return true;
    }
    const std::vector<eda::WaveSignal>& Signals() const override { return signals_; }
    std::string Timescale() const override { return "1ns"; }
    eda::WaveTimeRange TimeRange() const override { return eda::WaveTimeRange{0, 1000, true}; }
    bool Query(int signalId, std::uint64_t t0, std::uint64_t t1,
               std::vector<eda::WaveTransition>& out, std::string&) override {
        out.clear();
        if (signalId != 0) return true;  // 只有 sig0 有跳变。
        // 64 位边界：t0 可能是 UINT64_MAX，递增前必须显式防回绕。
        std::uint64_t tick = t0;
        for (int i = 0; i < 4 && tick <= t1; ++i) {
            out.push_back(eda::WaveTransition{tick, (i % 2 == 0) ? "1" : "0"});
            if (tick == std::numeric_limits<std::uint64_t>::max()) break;
            ++tick;
        }
        return true;
    }
    bool ValueAt(int signalId, std::uint64_t t, std::string& value, std::string&) override {
        if (signalId != 0) {
            value.clear();
            return true;
        }
        value = (t % 2 == 0) ? "1" : "0";
        return true;
    }

private:
    int signalCount_ = 0;
    std::vector<eda::WaveSignal> signals_;
};

// ---------------------------------------------------------------------------
// HTTP 助手（真实 loopback；除批量提交外每个请求独立连接）。
// ---------------------------------------------------------------------------
eda::Json ParseBody(const httplib::Result& res, int& status) {
    if (!res) {
        status = -1;
        return eda::Json::object();
    }
    status = res->status;
    try {
        return eda::Json::parse(res->body);
    } catch (const std::exception&) {
        return eda::Json::object();
    }
}

void AddToken(httplib::Headers& headers, const std::string& token) {
    if (!token.empty()) headers.emplace("Authorization", "Bearer " + token);
}

eda::Json GetJson(int port, const std::string& path, const std::string& token, int& status,
                  const httplib::Headers& extra = httplib::Headers(), int readTimeout = 6) {
    httplib::Client client("127.0.0.1", port);
    client.set_connection_timeout(2, 0);
    client.set_read_timeout(readTimeout, 0);
    httplib::Headers headers = extra;
    AddToken(headers, token);
    return ParseBody(client.Get(path.c_str(), headers), status);
}

eda::Json PostJson(int port, const std::string& path, const std::string& token,
                   const eda::Json& body, int& status,
                   const httplib::Headers& extra = httplib::Headers(), int readTimeout = 10) {
    httplib::Client client("127.0.0.1", port);
    client.set_connection_timeout(2, 0);
    client.set_read_timeout(readTimeout, 0);
    httplib::Headers headers = extra;
    AddToken(headers, token);
    return ParseBody(client.Post(path.c_str(), headers, body.dump(), "application/json"), status);
}

// 原始正文（体量边界用例）。
eda::Json PostRaw(int port, const std::string& path, const std::string& token,
                  const std::string& raw, int& status, int readTimeout = 10) {
    httplib::Client client("127.0.0.1", port);
    client.set_connection_timeout(2, 0);
    client.set_read_timeout(readTimeout, 0);
    httplib::Headers headers{{"Content-Type", "application/json"}};
    AddToken(headers, token);
    return ParseBody(client.Post(path.c_str(), headers, raw, "application/json"), status);
}

// 复用同一连接（批量提交用；避免 500+ 次建连造成的抖动）。
eda::Json PostJsonOn(httplib::Client& client, const std::string& path, const std::string& token,
                     const eda::Json& body, int& status,
                     const httplib::Headers& extra = httplib::Headers()) {
    httplib::Headers headers = extra;
    AddToken(headers, token);
    return ParseBody(client.Post(path.c_str(), headers, body.dump(), "application/json"), status);
}

// ---------------------------------------------------------------------------
// schema 断言助手。
// ---------------------------------------------------------------------------
void CheckSchema(eda::SimpleSchemaRegistry& registry, const std::string& schemaId,
                 const eda::Json& document, const std::string& label) {
    std::string reason;
    const bool ok = registry.Validate(schemaId, document, reason);
    Check(ok, label + " data validates against " + schemaId);
    if (!ok) {
        std::cout << "      schema reason: " << reason << "\n";
        std::cout << "      document: " << document.dump().substr(0, 400) << "\n";
    }
}

// 逐条校验集合元素（$ref 不被注册表解析，所以集合必须自己展开）。
void CheckEach(eda::SimpleSchemaRegistry& registry, const std::string& schemaId,
               const eda::Json& array, const std::string& label) {
    bool sawAny = false;
    bool allOk = array.is_array();
    if (array.is_array()) {
        for (const auto& item : array) {
            std::string reason;
            sawAny = true;
            if (!registry.Validate(schemaId, item, reason)) {
                allOk = false;
                std::cout << "      schema reason: " << reason << "\n";
            }
        }
    }
    Check(allOk && sawAny, label + " (every element validates against " + schemaId + ")");
}

// ---------------------------------------------------------------------------
// 工程夹具。
// ---------------------------------------------------------------------------
void WriteFile(const std::filesystem::path& path, const std::string& text) {
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}

std::string ManifestWithSources(const std::vector<std::string>& sources) {
    const eda::Json document{
        {"build", eda::Json{{"top_module", eda::Json::array({"top"})}}},
        {"paths", eda::Json{{"source_files", sources}}},
        {"fpga", eda::Json{{"target_profile", "test-target"}, {"yosys_strategy", "baseline"}}}};
    return document.dump(2) + "\n";
}

std::string ResolveSchemaDir(int argc, char** argv) {
    if (argc > 1) return argv[1];
#if defined(SIGFLOW_REPO_ROOT)
    return std::string(SIGFLOW_REPO_ROOT) + "/contracts/edu-agent/v1";
#else
    return "contracts/edu-agent/v1";
#endif
}

bool LoadJsonFile(const std::string& path, eda::Json& out) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return false;
    const std::string content((std::istreambuf_iterator<char>(input)),
                              std::istreambuf_iterator<char>());
    try {
        out = eda::Json::parse(content);
    } catch (const std::exception&) {
        return false;
    }
    return true;
}

void RegisterSchemaFile(eda::SimpleSchemaRegistry& registry, const std::string& schemaDir,
                        const char* id, const char* fileName) {
    eda::Json schema;
    const bool loaded = LoadJsonFile(schemaDir + "/schemas/" + fileName, schema);
    const bool registered = loaded && registry.RegisterSchema(id, schema);
    Check(registered, std::string("register schema ") + id + " from " + fileName);
}

// $ref 解析：文件引用按文件名映射到 <契约目录>/schemas/<name>；内部指针由注册表解析。
eda::SimpleSchemaRegistry::RefResolver MakeFileResolver(const std::string& schemaDir) {
    return [schemaDir](const std::string& reference, eda::Json& out) {
        const std::size_t hash = reference.find('#');
        const std::string filePart =
            hash == std::string::npos ? reference : reference.substr(0, hash);
        if (filePart.empty()) return false;
        const std::size_t slash = filePart.find_last_of("/\\");
        const std::string name = slash == std::string::npos ? filePart : filePart.substr(slash + 1);
        return LoadJsonFile(schemaDir + "/schemas/" + name, out) && out.is_object();
    };
}

}  // namespace

int main(int argc, char** argv) {
    namespace fs = std::filesystem;

    const std::string schemaDir = ResolveSchemaDir(argc, argv);
    std::cout << "[NG-10] contract schema dir: " << schemaDir << "\n";

    eda::SimpleSchemaRegistry registry;
    // 严格契约校验：解析 $ref（文件引用 + 内部指针），嵌套对象的 required/enum/const 一并生效。
    registry.SetRefResolver(MakeFileResolver(schemaDir));
    registry.SetStrictRefs(true);
    RegisterSchemaFile(registry, schemaDir, "capabilities.json", "capabilities.schema.json");
    RegisterSchemaFile(registry, schemaDir, "agent-health.json", "agent-health.schema.json");
    RegisterSchemaFile(registry, schemaDir, "project-context.json", "project-context.schema.json");
    RegisterSchemaFile(registry, schemaDir, "snapshot.json", "snapshot.schema.json");
    RegisterSchemaFile(registry, schemaDir, "design-node.json", "design-node.schema.json");
    RegisterSchemaFile(registry, schemaDir, "context-query.json", "context-query.schema.json");
    RegisterSchemaFile(registry, schemaDir, "legacy-job.json", "legacy-job.schema.json");
    RegisterSchemaFile(registry, schemaDir, "job-report-view.json", "job-report-view.schema.json");
    RegisterSchemaFile(registry, schemaDir, "project-state.json", "project-state.schema.json");
    RegisterSchemaFile(registry, schemaDir, "event.json", "event.schema.json");
    RegisterSchemaFile(registry, schemaDir, "wave-signals.json", "wave-signals.schema.json");
    RegisterSchemaFile(registry, schemaDir, "wave-query.json", "wave-query.schema.json");
    RegisterSchemaFile(registry, schemaDir, "envelope.json", "envelope.schema.json");

    // ---- 临时工程夹具 ----
    const fs::path root = fs::temp_directory_path() /
                          ("sigflow_schema_runtime_" +
                           std::to_string(std::chrono::high_resolution_clock::now()
                                              .time_since_epoch()
                                              .count()));
    std::error_code cleanupError;
    fs::remove_all(root, cleanupError);
    fs::create_directories(root);

    const fs::path projectA = root / "projA";
    WriteFile(projectA / "rtl" / "top.v", "module top;\nendmodule\n");
    WriteFile(projectA / "sigflow.project", ManifestWithSources({"rtl/top.v"}));

    // 3 MiB 源文件：/sources/{id} 无内容预算，用于触发 2 MiB 响应护栏。
    const fs::path projectBig = root / "projBig";
    const std::string bigSource(3u * 1024u * 1024u, 'x');
    WriteFile(projectBig / "rtl" / "big.v", bigSource);
    WriteFile(projectBig / "sigflow.project", ManifestWithSources({"rtl/big.v"}));

    // 波形 artifact 的登记文件（backend 只要求文件可打开）。
    const fs::path waveArtifactFile = root / "wave.vcd";
    WriteFile(waveArtifactFile, "$timescale 1ns $end\n");

    eda::agent::GatewayConfig config;
    config.instanceId = "inst-schema-runtime";
    config.edition = "edu";
    config.protocolVersion = "edu.api.v1";
    config.token = "agent-token";
    config.uiToken = "ui-token";
    config.port = 0;

    const auto provider = []() {
        std::vector<eda::agent::ReadyPlugin> ready;
        ready.push_back({"eda-synth-yosys", "1.0.0", {"synth"}});
        ready.push_back({"eda-sim-verilator", "1.0.0", {"sim.build", "sim.run"}});
        return ready;
    };

    eda::agent::GatewayServer server(config, provider);
    FakeJobService fakeJobs;
    server.SetJobServiceProvider([&fakeJobs]() -> eda::IJobService* { return &fakeJobs; });
    server.SetWaveformBackendFactory([]() -> std::shared_ptr<eda::IWaveformBackend> {
        return std::make_shared<WideWaveBackend>(5000);
    });
    eda::agent::WaveArtifactRecord waveRecord;
    waveRecord.artifactId = "wave-art-1";
    waveRecord.projectId = "project-wave";
    waveRecord.revision = "rev-1-000000000000";
    waveRecord.jobId = "job-wave";
    waveRecord.path = waveArtifactFile;
    waveRecord.sha256 = "";  // 空 = 不做 hash 校验（只要求文件存在）。
    waveRecord.schema = "edu.wave.vcd.v1";
    server.RegisterWaveArtifact(waveRecord);

    std::string error;
    std::string projectAId;
    Check(server.RefreshProjectSnapshotStateFromDisk(projectA, false, true, projectAId, error),
          "gateway builds project A context from sigflow.project");
    std::string projectBigId;
    Check(server.RefreshProjectSnapshotStateFromDisk(projectBig, false, true, projectBigId, error),
          "gateway builds big-source project context");
    Check(server.Start(error), "gateway starts and binds loopback");
    if (!server.Running()) {
        std::cout << "      start error: " << error << "\n";
        std::cout << "FAILURES\n";
        return 1;
    }
    server.RunAsync();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    const int port = server.Port();
    const std::string token = "agent-token";
    const std::string baseA = "/api/v1/projects/" + projectAId;
    const std::string bigBase = "/api/v1/projects/" + projectBigId;
    int status = 0;

    // 实测汇总变量（只填真正观测到的值，末尾一次性打印）。
    std::string healthInstanceId;
    std::string healthProtocolVersion;
    std::string healthEdition;
    std::string healthBuild;
    int underStatus = 0;
    int overStatus = 0;
    std::size_t underBytes = 0;
    std::size_t overBytes = 0;
    int responseCapStatus = 0;
    std::string responseLimitBytes;
    std::string responseActualBytes;
    const std::size_t bigSourceBytes = bigSource.size();
    std::size_t eventsDefaultPage = 0;
    std::size_t eventsMaxPage = 0;
    std::size_t jobsDefaultPage = 0;
    std::size_t jobsMaxPage = 0;
    std::size_t jobsSubmitted = 0;
    std::size_t waveDefaultPage = 0;
    std::size_t waveMaxPage = 0;
    std::size_t waveTotal = 0;
    long long legacyDefaultLimit = 0;
    long long legacyMaxLimit = 0;
    long long legacyZeroLimit = 0;
    std::string stateHighWatermark;
    int waveBoundaryStatus = 0;
    std::size_t concurrentRequests = 0;
    std::size_t concurrentFailures = 0;
    int followUpHealth = 0;
    int unknownPathStatus = 0;
    std::string unknownPathCode;
    int wrongMethodStatus = 0;
    std::string wrongMethodCode;
    bool utf8HostMatches = false;
    bool utf8Loaded = false;
    int utf8SourcesStatus = 0;
    std::size_t trimEvents = 0;
    std::string trimmedOldest;
    std::string trimmedHighWatermark;
    int expiredStatus = 0;
    int retainedStatus = 0;

    // =====================================================================
    // A. 真实响应 data ↔ 契约 schema
    // =====================================================================
    std::cout << "\n[A] live-response schema validation\n";

    // A1. GET /health
    // 路由事实：GatewayServer.cpp Start() -> BuildHealthData()，
    //   data = {instance_id, protocol_version, edition, build}（没有 capabilities）。
    // contracts/ 下没有 gateway /health 的专用 schema 文件：
    //   * capabilities.schema.json 形状最接近，但它 required "capabilities"；
    //   * agent-health.schema.json 描述的是 NG-07 Python sidecar 的 /health
    //     （required protocol/agent_version/ready/model），不是本路由的 data。
    // 因此这里按 openapi components.schemas.HealthData（required: instance_id,
    // protocol_version, edition, build）校验四个字段本身，并把"没有专用 schema"这一
    // **观测到的**事实显式断言出来——不新造 schema、也不放宽已有 schema。
    {
        const eda::Json body = GetJson(port, "/api/v1/health", "", status);
        Check(status == 200, "GET /health returns 200");
        Check(EnvelopeOk(body), "GET /health envelope carries schema_version/request_id/trace_id");
        const eda::Json& health = DataOf(body);
        healthInstanceId = Str(health, "instance_id");
        healthProtocolVersion = Str(health, "protocol_version");
        healthEdition = Str(health, "edition");
        healthBuild = Str(health, "build");
        Check(health.is_object() && healthInstanceId == "inst-schema-runtime",
              "health data carries instance_id");
        Check(healthProtocolVersion == "edu.api.v1", "health data carries protocol_version");
        Check(healthEdition == "edu" && (At(health, "build").is_string() ||
                                         At(health, "build").is_null()),
              "health data carries edition/build");
        Check(health.size() == 4 && !health.contains("capabilities"),
              "health data has exactly the four HealthData fields (no dedicated schema file)");
        {
            std::string reason;
            const bool ok = registry.Validate("capabilities.json", health, reason);
            Check(!ok && reason.find("capabilities") != std::string::npos,
                  "capabilities.schema.json does not fit /health (missing required field "
                  "'capabilities')");
            std::cout << "      observed: " << reason << "\n";
            // 子集探针：把 health 的四个字段按其声明的 property schema 过一遍
            // （补一个空的 capabilities 只为满足顶层 required）。
            eda::Json probe = health;
            probe["capabilities"] = eda::Json::array();
            std::string probeReason;
            const bool probeOk = registry.Validate("capabilities.json", probe, probeReason);
            Check(probeOk,
                  "the four HealthData fields satisfy capabilities.schema.json property types");
            if (!probeOk) std::cout << "      observed: " << probeReason << "\n";
        }
        {
            std::string reason;
            const bool ok = registry.Validate("agent-health.json", health, reason);
            Check(!ok, "agent-health.schema.json is the sidecar schema, not this route's data");
            std::cout << "      observed: " << reason << "\n";
        }
    }

    // A2. GET /capabilities
    {
        const eda::Json body = GetJson(port, "/api/v1/capabilities", token, status);
        Check(status == 200, "GET /capabilities returns 200");
        Check(EnvelopeOk(body), "GET /capabilities envelope carries the common fields");
        CheckSchema(registry, "capabilities.json", DataOf(body), "GET /capabilities");
    }

    // A3. GET /projects/{p}/context
    std::string revision;
    std::string sourceId;
    std::string sourceHash;
    std::string sourcePath;
    {
        const eda::Json body = GetJson(port, baseA + "/context", token, status);
        Check(status == 200, "GET /projects/{p}/context returns 200");
        Check(EnvelopeOk(body), "GET /context envelope carries the common fields");
        CheckSchema(registry, "project-context.json", DataOf(body), "GET /context");
        revision = Str(DataOf(body), "revision");
        const eda::Json& sources = At(DataOf(body), "sources");
        if (sources.is_array() && !sources.empty()) {
            sourceId = Str(sources[0], "source_id");
            sourceHash = Str(sources[0], "sha256");
            sourcePath = Str(sources[0], "path");
        }
        Check(!revision.empty() && !sourceId.empty() && !sourceHash.empty(),
              "context exposes revision and source identity");
    }

    // A4. POST /projects/{p}/snapshots
    std::string snapshotId;
    {
        const eda::Json body =
            PostJson(port, baseA + "/snapshots", token,
                     eda::Json{{"expected_revision", revision}, {"require_saved", true}}, status);
        Check(status == 201, "POST /projects/{p}/snapshots returns 201");
        Check(EnvelopeOk(body), "POST /snapshots envelope carries the common fields");
        CheckSchema(registry, "snapshot.json", DataOf(body), "POST /snapshots");
        snapshotId = Str(DataOf(body), "snapshot_id");
    }

    // A5. GET /projects/{p}/design/nodes/{id}（宿主先注入不可变映射）
    {
        eda::agent::DesignNodeRecord node;
        node.nodeId = "node-top";
        node.kind = "module";
        node.name = "top";
        node.moduleName = "top";
        node.instancePath = "top";
        eda::agent::DesignSourceRef ref;
        ref.sourceId = sourceId;
        ref.path = sourcePath;
        ref.fileHash = sourceHash;
        ref.startLine = 1;
        ref.endLine = 2;
        node.sourceRefs.push_back(ref);
        eda::agent::DesignPort nodePort;
        nodePort.name = "clk";
        nodePort.direction = "input";
        nodePort.width = 1;
        node.ports.push_back(nodePort);
        server.SetDesignNodes(projectAId, revision, {node});

        const eda::Json body = GetJson(port, baseA + "/design/nodes/node-top", token, status);
        Check(status == 200, "GET /projects/{p}/design/nodes/{id} returns 200");
        Check(EnvelopeOk(body), "GET /design/nodes envelope carries the common fields");
        CheckSchema(registry, "design-node.json", DataOf(body), "GET /design/nodes/{id}");
        Check(Str(DataOf(body), "mapping_status") == "available",
              "design node with a verified source ref maps as available");
    }

    // A6. POST /projects/{p}/context/query
    {
        const eda::Json body = PostJson(
            port, baseA + "/context/query", token,
            eda::Json{{"needs", eda::Json::array({eda::Json{{"kind", "sources"},
                                                           {"source_id", sourceId}}})}},
            status);
        Check(status == 200, "POST /projects/{p}/context/query returns 200");
        Check(EnvelopeOk(body), "POST /context/query envelope carries the common fields");
        CheckSchema(registry, "context-query.json", DataOf(body), "POST /context/query");
    }

    // A7. GET /projects/{p}/state
    {
        const eda::Json body = GetJson(port, baseA + "/state", token, status);
        Check(status == 200, "GET /projects/{p}/state returns 200");
        Check(EnvelopeOk(body), "GET /state envelope carries the common fields");
        CheckSchema(registry, "project-state.json", DataOf(body), "GET /state");
        stateHighWatermark = Str(DataOf(body), "high_watermark");
        Check(IsDecimalString(At(DataOf(body), "high_watermark")) &&
                  IsDecimalString(At(DataOf(body), "oldest_sequence")),
              "state exposes high_watermark/oldest_sequence as decimal strings");
    }

    // A8. GET /projects/{p}/events（逐条 event 校验）
    {
        server.PublishEvent("project/saved", projectAId, "trace-schema-1",
                            eda::Json{{"revision", revision}});
        server.PublishEvent("job/finished", projectAId, "trace-schema-2",
                            eda::Json{{"job_id", "job-x"}});
        const eda::Json body = GetJson(port, baseA + "/events?after=0&wait_ms=0", token, status);
        Check(status == 200, "GET /projects/{p}/events returns 200");
        Check(EnvelopeOk(body), "GET /events envelope carries the common fields");
        Check(Count(At(DataOf(body), "events")) >= 2, "events stream returns appended events");
        CheckEach(registry, "event.json", At(DataOf(body), "events"),
                  "GET /events stream");
    }

    // A9. NG-09 旧历史：先造出只读旧记录，再校验列表与报告。
    {
        const fs::path legacyJob1 = projectA / ".sigflow" / "jobs" / "synth" / "legacy-1";
        const eda::Json legacyManifest{
            {"job_type", "synth"},
            {"state", "Succeeded"},
            {"created_at", "2026-01-01T00:00:00Z"},
            {"updated_at", "2026-01-01T00:01:00Z"},
            {"parameters", eda::Json{{"project_path", "C:/should-not-leak"}}}};
        WriteFile(legacyJob1 / "manifest.json", legacyManifest.dump(2) + "\n");
        const eda::Json legacyReport{
            {"schema_version", "1.0"},
            {"job_id", "legacy-1"},
            {"job_type", "synth"},
            {"state", "Succeeded"},
            {"exit_code", 0},
            {"errors", eda::Json::array({eda::Json{{"code", "LATCH_INFERRED"},
                                                   {"severity", "warning"},
                                                   {"stage", "synth"},
                                                   {"summary", "demo diagnostic"},
                                                   {"log_line", 7}}})},
            {"artifacts", eda::Json::array({eda::Json{{"kind", "netlist"},
                                                       {"sha256", std::string(64, 'a')},
                                                       {"path", "C:/should-not-leak/out.json"}}})}};
        WriteFile(legacyJob1 / "reports" / "job-report.json", legacyReport.dump(2) + "\n");
        // 只有 manifest：problem=report_missing。
        WriteFile(projectA / ".sigflow" / "jobs" / "synth" / "legacy-2" / "manifest.json",
                  eda::Json{{"job_type", "synth"}, {"state", "Failed"}}.dump(2) + "\n");
        // 什么都没有：problem=manifest_unreadable。
        fs::create_directories(projectA / ".sigflow" / "jobs" / "synth" / "legacy-3");

        const eda::Json body = GetJson(port, baseA + "/legacy/jobs", token, status);
        Check(status == 200, "GET /projects/{p}/legacy/jobs returns 200");
        Check(EnvelopeOk(body), "GET /legacy/jobs envelope carries the common fields");
        Check(Str(DataOf(body), "origin") == "legacy" &&
                  Str(DataOf(body), "completeness") == "legacy_unverified",
              "legacy list is marked legacy_unverified");
        Check(Count(At(DataOf(body), "jobs")) == 3,
              "legacy list returns the three on-disk records");
        CheckEach(registry, "legacy-job.json", At(DataOf(body), "jobs"), "GET /legacy/jobs");
        Check(body.dump().find("should-not-leak") == std::string::npos,
              "legacy list never leaks local manifest paths");

        const eda::Json reportBody =
            GetJson(port, baseA + "/legacy/jobs/legacy-1/report", token, status);
        Check(status == 200, "GET /legacy/jobs/{id}/report returns 200");
        Check(EnvelopeOk(reportBody), "GET /legacy/jobs/{id}/report envelope carries the fields");
        CheckSchema(registry, "job-report-view.json", DataOf(reportBody),
                    "GET /legacy/jobs/{id}/report");
        Check(Str(DataOf(reportBody), "origin") == "legacy" &&
                  Str(DataOf(reportBody), "completeness") == "legacy_unverified",
              "legacy report keeps origin/completeness markers");
        Check(At(DataOf(reportBody), "artifacts").dump().find("should-not-leak") ==
                  std::string::npos,
              "normalized legacy report never leaks local artifact paths");
    }

    // A10. 提交真实 Agent Job 后校验 GET /jobs/{j}/report（假 IJobService）。
    std::string grantId;
    std::string firstJobId;
    {
        const eda::Json issued =
            PostJson(port, baseA + "/grants", "ui-token",
                     eda::Json{{"plan_hash", "ph-schema"},
                               {"revision", revision},
                               {"snapshot_id", snapshotId},
                               {"max_jobs", 16}},
                     status);
        Check(status == 201, "UI identity can issue a grant for the report/pagination flow");
        grantId = Str(DataOf(issued), "grant_id");

        httplib::Headers idem;
        idem.emplace("Idempotency-Key", "schema-key-0");
        const eda::Json submitted =
            PostJson(port, baseA + "/jobs", token,
                     eda::Json{{"snapshot_id", snapshotId},
                               {"expected_revision", revision},
                               {"capability", "eda.synth"},
                               {"grant_id", grantId},
                               {"params", eda::Json{{"top_module", "top"}}}},
                     status, idem);
        Check(status == 202, "POST /projects/{p}/jobs returns 202");
        firstJobId = Str(DataOf(submitted), "job_id");
        Check(!firstJobId.empty(), "job submission returns a job_id");

        const eda::Json body =
            GetJson(port, "/api/v1/jobs/" + firstJobId + "/report", token, status);
        Check(status == 200, "GET /jobs/{j}/report returns 200");
        Check(EnvelopeOk(body), "GET /jobs/{j}/report envelope carries the common fields");
        CheckSchema(registry, "job-report-view.json", DataOf(body), "GET /jobs/{j}/report");
    }

    // =====================================================================
    // B. 边界/限额矩阵
    // =====================================================================
    std::cout << "\n[B] boundary / limit matrix\n";

    // B1/B2. 请求体 1 MiB（config.maxRequestBodyBytes）
    {
        constexpr std::size_t kLimit = 1024 * 1024;
        const auto bodyOfSize = [](std::size_t total) {
            const std::string prefix = "{\"needs\":[],\"pad\":\"";
            const std::string suffix = "\"}";
            return prefix + std::string(total - prefix.size() - suffix.size(), 'a') + suffix;
        };
        const std::string under = bodyOfSize(kLimit - 4096);
        underBytes = under.size();
        const eda::Json underBody =
            PostRaw(port, baseA + "/context/query", token, under, underStatus);
        Check(underStatus == 200,
              "request body just under 1 MiB is accepted (not 413) on POST /context/query");
        Check(EnvelopeOk(underBody) && At(DataOf(underBody), "used_bytes").is_number(),
              "under-limit request returns a real success payload");
        std::cout << "      observed: " << underBytes << " bytes -> status " << underStatus << "\n";

        const std::string over = bodyOfSize(kLimit + 4096);
        overBytes = over.size();
        const eda::Json overBody = PostRaw(port, baseA + "/context/query", token, over, overStatus);
        Check(overStatus == 413, "request body over 1 MiB is rejected with 413");
        Check(ErrorCode(overBody) == "RESOURCE_EXHAUSTED",
              "oversized request maps to RESOURCE_EXHAUSTED");
        std::cout << "      observed: " << overBytes << " bytes -> status " << overStatus << "/"
                  << ErrorCode(overBody) << "\n";
    }

    // B3. 响应 2 MiB 护栏（config.maxResponseBodyBytes）
    // 路由选择：GET /projects/{p}/sources/{id} 对已登记源码做无载荷预算的全文读取；
    // 一个 3 MiB 的已登记源即可越过 2 MiB 护栏（/artifacts content 另有 256 KiB 预算，
    // 波形查询另有 2 MiB 估算预算，都先于该护栏生效）。
    {
        const eda::Json context = GetJson(port, bigBase + "/context", token, status);
        const std::string bigSourceId = Str(Item(At(DataOf(context), "sources"), 0), "source_id");
        Check(status == 200 && !bigSourceId.empty(), "big-source project exposes its source id");
        const eda::Json body =
            GetJson(port, bigBase + "/sources/" + bigSourceId, token, status);
        responseCapStatus = status;
        Check(status == 429, "response over 2 MiB is refused with 429 (not truncated)");
        Check(ErrorCode(body) == "RESOURCE_EXHAUSTED",
              "oversized response carries RESOURCE_EXHAUSTED");
        const eda::Json& details = At(At(body, "error"), "details");
        responseLimitBytes = Str(details, "limit_bytes");
        responseActualBytes = Str(details, "actual_bytes");
        Check(details.is_object() && IsDecimalString(At(details, "limit_bytes")) &&
                  IsDecimalString(At(details, "actual_bytes")),
              "oversized response carries decimal details.limit_bytes/actual_bytes");
        Check(responseLimitBytes == std::to_string(config.maxResponseBodyBytes) &&
                  !responseActualBytes.empty() &&
                  std::stoull(responseActualBytes) > std::stoull(responseLimitBytes),
              "details report the configured limit and the real payload size");
        std::cout << "      observed: limit_bytes=" << responseLimitBytes
                  << " actual_bytes=" << responseActualBytes << " status=" << responseCapStatus
                  << " retryable=" << Bool(At(body, "error"), "retryable", false) << "\n";
    }

    // B4. 集合分页：/events（默认 100、最多 500）
    constexpr std::size_t kPageEvents = 600;
    {
        for (std::size_t i = 0; i < kPageEvents; ++i) {
            server.PublishEvent("agent/state", "prj-page", "trace-page",
                                eda::Json{{"index", static_cast<int>(i)}});
        }
        const eda::Json page =
            GetJson(port, "/api/v1/projects/prj-page/events?after=0&wait_ms=0", token, status);
        eventsDefaultPage = Count(At(DataOf(page), "events"));
        Check(status == 200 && eventsDefaultPage == 100 && Bool(DataOf(page), "has_more", false),
              "events collection defaults to a 100-event page with has_more");
        const eda::Json maxPage = GetJson(
            port, "/api/v1/projects/prj-page/events?after=0&limit=99999&wait_ms=0", token, status);
        eventsMaxPage = Count(At(DataOf(maxPage), "events"));
        Check(status == 200 && eventsMaxPage == 500 && Bool(DataOf(maxPage), "has_more", false),
              "events limit=99999 is clamped to 500 with has_more");
        const eda::Json zeroPage = GetJson(
            port, "/api/v1/projects/prj-page/events?after=0&limit=0&wait_ms=0", token, status);
        Check(status == 200 && Count(At(DataOf(zeroPage), "events")) == 100,
              "events limit=0 falls back to the 100 default");
        const eda::Json onePage = GetJson(
            port, "/api/v1/projects/prj-page/events?after=0&limit=1&wait_ms=0", token, status);
        Check(status == 200 && Count(At(DataOf(onePage), "events")) == 1 &&
                  Bool(DataOf(onePage), "has_more", false),
              "events honors an explicit small limit and reports has_more");
    }

    // B4b. 集合分页：/projects/{p}/jobs（默认 100、最多 500）—— 真提交 505 个 Job
    // （A10 已提交 1 个，共 506 个；要观测 500 的夹紧必须真的超过 500 条）。
    {
        httplib::Client jobsClient("127.0.0.1", port);
        jobsClient.set_connection_timeout(2, 0);
        jobsClient.set_read_timeout(10, 0);
        const std::string jobsRoute = baseA + "/jobs";
        std::size_t submitted = 1;  // A10 已提交 1 个
        for (std::size_t i = 1; i <= 505; ++i) {
            // Keep the real 16-job approval limit: pagination does not justify
            // a privileged 506-job grant. Approve each bounded batch separately.
            if (i % 16 == 0) {
                const auto batch = PostJson(port, baseA + "/grants", "ui-token",
                    eda::Json{{"plan_hash", "ph-schema"}, {"revision", revision},
                              {"snapshot_id", snapshotId}, {"max_jobs", 16}}, status);
                Check(status == 201, "next bounded pagination batch approved by UI");
                grantId = Str(DataOf(batch), "grant_id");
            }
            httplib::Headers key;
            key.emplace("Idempotency-Key", "schema-page-" + std::to_string(i));
            PostJsonOn(jobsClient, jobsRoute, token,
                       eda::Json{{"snapshot_id", snapshotId},
                                 {"expected_revision", revision},
                                 {"capability", "eda.synth"},
                                 {"grant_id", grantId},
                                 {"params", eda::Json{{"top_module", "top"}}}},
                       status, key);
            if (status == 202) {
                ++submitted;
            } else if (i == 1) {
                std::cout << "      job submit status=" << status << "\n";
            }
        }
        jobsSubmitted = submitted;
        Check(submitted == 506, "506 agent jobs submitted through the real route");

        const eda::Json page = GetJson(port, jobsRoute, token, status);
        jobsDefaultPage = Count(At(DataOf(page), "jobs"));
        Check(status == 200 && Int(DataOf(page), "total", 0) == 506,
              "project job listing reports every submitted job");
        Check(jobsDefaultPage == 100 && Bool(DataOf(page), "has_more", false),
              "project job listing defaults to a 100-job page with has_more");
        const eda::Json maxPage = GetJson(port, jobsRoute + "?limit=99999", token, status);
        jobsMaxPage = Count(At(DataOf(maxPage), "jobs"));
        Check(status == 200 && jobsMaxPage == 500 && Bool(DataOf(maxPage), "has_more", false),
              "project job limit=99999 is clamped to 500 with has_more");
        const eda::Json offsetPage =
            GetJson(port, jobsRoute + "?limit=10&offset=505", token, status);
        Check(status == 200 && Count(At(DataOf(offsetPage), "jobs")) == 1 &&
                  !Bool(DataOf(offsetPage), "has_more", true),
              "project job offset paging returns the remaining page");
    }

    // B4c. 波形信号列表：默认 256、最多 4096；并校验 wave-signals.schema.json。
    {
        const eda::Json page = GetJson(port, "/api/v1/waves/wave-art-1/signals", token, status);
        Check(status == 200, "GET /waves/{a}/signals returns 200");
        Check(EnvelopeOk(page), "GET /waves/{a}/signals envelope carries the common fields");
        CheckSchema(registry, "wave-signals.json", DataOf(page), "GET /waves/{a}/signals");
        waveDefaultPage = Count(At(DataOf(page), "signals"));
        waveTotal = static_cast<std::size_t>(Int(DataOf(page), "total", 0));
        Check(waveDefaultPage == 256 && waveTotal == 5000 && Bool(DataOf(page), "has_more", false),
              "wave signal list defaults to a 256-signal page of 5000");
        // 契约要求 time_range 为 {start_tick,end_tick} 十进制字符串（时间范围不可信时为 null）。
        // SimpleSchemaRegistry 不强制 additionalProperties，所以这里显式断言字段名与类型。
        Check(At(At(DataOf(page), "time_range"), "start_tick").is_string() &&
                  At(At(DataOf(page), "time_range"), "end_tick").is_string() &&
                  !At(At(DataOf(page), "time_range"), "begin").is_number(),
              "wave signal page emits the contracted time_range{start_tick,end_tick} strings");
        const eda::Json maxPage =
            GetJson(port, "/api/v1/waves/wave-art-1/signals?limit=99999", token, status);
        waveMaxPage = Count(At(DataOf(maxPage), "signals"));
        Check(status == 200 && waveMaxPage == 4096 && Bool(DataOf(maxPage), "has_more", false),
              "wave signal limit=99999 is clamped to 4096 with has_more");
    }

    // B4d. 旧历史列表：limit 默认 100 / 最多 500（路由回显 limit，可直接断言夹紧）。
    {
        const eda::Json defaults = GetJson(port, baseA + "/legacy/jobs", token, status);
        legacyDefaultLimit = Int(DataOf(defaults), "limit", -1);
        Check(status == 200 && legacyDefaultLimit == 100, "legacy job list defaults to limit=100");
        const eda::Json maxPage = GetJson(port, baseA + "/legacy/jobs?limit=99999", token, status);
        legacyMaxLimit = Int(DataOf(maxPage), "limit", -1);
        Check(status == 200 && legacyMaxLimit == 500,
              "legacy job list clamps limit=99999 to 500");
        const eda::Json zeroPage = GetJson(port, baseA + "/legacy/jobs?limit=0", token, status);
        legacyZeroLimit = Int(DataOf(zeroPage), "limit", -1);
        Check(status == 200 && legacyZeroLimit == 1, "legacy job list clamps limit=0 up to 1");
    }

    // B5. 64 位值：/state 的十进制字符串 + 波形查询的 uint64 边界 tick。
    {
        const eda::Json state = GetJson(port, baseA + "/state", token, status);
        stateHighWatermark = Str(DataOf(state), "high_watermark");
        Check(status == 200 && IsDecimalString(At(DataOf(state), "high_watermark")),
              "/state high_watermark is a decimal string");
        constexpr std::uint64_t kMaxTick = std::numeric_limits<std::uint64_t>::max();
        const eda::Json query =
            PostJson(port, "/api/v1/waves/wave-art-1/query", token,
                     eda::Json{{"start_tick", kMaxTick},
                               {"end_tick", kMaxTick},
                               {"signals", eda::Json::array({0})}},
                     status);
        waveBoundaryStatus = status;
        Check(status == 200, "wave query at the unsigned 64-bit tick boundary returns 200");
        Check(Str(DataOf(query), "start_tick") == "18446744073709551615" &&
                  Str(DataOf(query), "end_tick") == "18446744073709551615",
              "wave query echoes the 64-bit boundary tick as a decimal string");
        CheckSchema(registry, "wave-query.json", DataOf(query), "POST /waves/{a}/query");
        const eda::Json& transitions =
            At(Item(At(DataOf(query), "signals"), 0), "transitions");
        Check(Count(transitions) > 0 &&
                  Str(Item(transitions, 0), "tick") == "18446744073709551615",
              "transition tick at UINT64_MAX survives as a decimal string");
    }

    // B6. 中文 + 空格相对路径（rtl/测试 模块/top.v）
    // 主机写入 manifest 的方式与 JsonProject::AddSourceFile 一致（file.generic_string()），
    // 先证明"写进 manifest 的字节 == 主机写出的字节"，以排除测试自身的编码偏差。
    {
        const fs::path projectUtf8 = root / "projUtf8";
        const std::string unicodeDir = "rtl/\u6d4b\u8bd5 \u6a21\u5757";  // rtl/测试 模块
        const std::string unicodeRel = unicodeDir + "/top.v";
        WriteFile(projectUtf8 / eda::platform::PathFromUtf8(unicodeRel),
                  "module top;\nendmodule\n");
        WriteFile(projectUtf8 / "sigflow.project", ManifestWithSources({unicodeRel}));
        const fs::path wideRel = eda::platform::PathFromUtf8(unicodeRel);
        Check(eda::platform::PathToUtf8(wideRel) == unicodeRel,
              "UTF-8 relative path round-trips through Platform::PathFromUtf8/PathToUtf8");
        utf8HostMatches = wideRel.generic_string() == unicodeRel;
        Check(utf8HostMatches,
              "host writes the manifest path as UTF-8 (generic_string() == the UTF-8 path)");
        Check(fs::is_regular_file(projectUtf8 / wideRel),
              "the UTF-8 source file really exists under rtl/测试 模块/");

        std::string unicodeProjectId;
        std::string unicodeError;
        utf8Loaded = server.RefreshProjectSnapshotStateFromDisk(projectUtf8, false, true,
                                                               unicodeProjectId, unicodeError);
        Check(utf8Loaded,
              "gateway loads a project whose sigflow.project records a non-ASCII UTF-8 relative "
              "source path");
        if (!utf8Loaded) {
            std::cout << "      FINDING: gateway refused the project: " << unicodeError << "\n";
            std::cout << "      FINDING: JsonProject::SourceFiles() builds std::filesystem::path "
                         "directly from the JSON string; on Windows/libstdc++ that widens every "
                         "byte (Platform.h documents PathFromUtf8 as mandatory there).\n";
        } else {
            const std::string utf8Base = "/api/v1/projects/" + unicodeProjectId;
            const eda::Json context = GetJson(port, utf8Base + "/context", token, status);
            Check(status == 200, "GET /context works for the non-ASCII path project");
            CheckSchema(registry, "project-context.json", DataOf(context),
                        "GET /context (non-ASCII path)");
            const eda::Json& sources = At(DataOf(context), "sources");
            Check(Count(sources) == 1 && Str(Item(sources, 0), "path") == unicodeRel,
                  "/context reports the exact UTF-8 relative path 'rtl/测试 模块/top.v'");
            const std::string utf8SourceId = Str(Item(sources, 0), "source_id");
            const eda::Json source =
                GetJson(port, utf8Base + "/sources/" + utf8SourceId, token, status);
            Check(status == 200, "GET /sources/{id} reads the non-ASCII path source");
            Check(Str(DataOf(source), "path") == unicodeRel &&
                      Str(DataOf(source), "text").find("module top") != std::string::npos,
                  "/sources/{id} returns that path's real content");
        }

        // B6b. 绕过 manifest 解析、由宿主直接注入不可变 DTO 时，同一份 UTF-8 相对路径
        // 仍然必须可读：/context 由 sourceSummary 原样回显，/sources/{id} 则要在路由内部
        // 用它拼绝对路径。两条断言把"上报"与"解析"两层分开定位。
        if (utf8HostMatches) {
            const std::string injectedSha = eda::platform::Sha256FileHex(projectUtf8 / wideRel);
            std::error_code sizeError;
            const std::uintmax_t injectedSizeValue =
                fs::file_size(projectUtf8 / wideRel, sizeError);
            const std::string injectedSize =
                sizeError ? std::string("0") : std::to_string(injectedSizeValue);
            Check(injectedSha.size() == 64, "test computes the injected source hash");
            eda::agent::ProjectSnapshotState injected;
            injected.request.projectId = "project-injected-utf8";
            injected.request.projectRoot = projectUtf8;
            injected.request.currentRevision = "rev-1-000000000000";
            injected.request.dirty = false;
            injected.request.synchronized = true;
            injected.request.top = "top";
            eda::agent::SnapshotSourceInput injectedInput;
            injectedInput.sourceId = "source-0000000000000000000000ab";
            injectedInput.relativePath = wideRel;
            injectedInput.expectedSha256 = injectedSha;
            injected.request.sources.push_back(injectedInput);
            injected.sourceSummary = eda::Json::array(
                {eda::Json{{"source_id", injectedInput.sourceId},
                           {"path", unicodeRel},
                           {"sha256", injectedSha},
                           {"size", injectedSize},
                           {"origin", "project"}}});
            Check(server.UpdateProjectSnapshotState(injected, error),
                  "host can inject project state carrying the UTF-8 relative path");
            const std::string injectedBase =
                "/api/v1/projects/" + injected.request.projectId;
            const eda::Json context = GetJson(port, injectedBase + "/context", token, status);
            Check(status == 200 &&
                      Str(Item(At(DataOf(context), "sources"), 0), "path") == unicodeRel,
                  "/context reports the host-injected exact UTF-8 relative path");
            const eda::Json source = GetJson(
                port, injectedBase + "/sources/" + injectedInput.sourceId, token, status);
            utf8SourcesStatus = status;
            Check(status == 200 &&
                      Str(DataOf(source), "text").find("module top") != std::string::npos,
                  "/sources/{id} reads a UTF-8 relative path (host-injected state)");
            if (status != 200) {
                std::cout << "      FINDING: GET /sources/{id} returned " << status << "/"
                          << ErrorCode(source) << " for path '" << unicodeRel << "'\n";
                std::cout << "      FINDING: the route joins the UTF-8 sourceSummary path with "
                             "the project root via fs::path(std::string), which widens every "
                             "byte instead of decoding UTF-8 (Platform::PathFromUtf8).\n";
            }
        }
    }

    // B7. 并发只读访问：8 线程混合 GET，全部 200，事后网关仍然可用。
    {
        constexpr int kThreads = 8;
        constexpr int kPerThread = 12;
        const std::vector<std::string> paths{
            "/api/v1/health",
            "/api/v1/capabilities",
            baseA + "/context",
            baseA + "/state",
            baseA + "/jobs",
            baseA + "/legacy/jobs",
            baseA + "/events?after=0&wait_ms=0",
        };
        std::atomic<std::size_t> requests{0};
        std::atomic<std::size_t> failures{0};
        std::vector<std::thread> workers;
        workers.reserve(kThreads);
        for (int t = 0; t < kThreads; ++t) {
            workers.emplace_back([&, t]() {
                httplib::Client client("127.0.0.1", port);
                client.set_connection_timeout(3, 0);
                client.set_read_timeout(10, 0);
                for (int i = 0; i < kPerThread; ++i) {
                    const std::string& path =
                        paths[static_cast<std::size_t>(t + i) % paths.size()];
                    httplib::Headers headers;
                    AddToken(headers, token);
                    const auto res = client.Get(path.c_str(), headers);
                    requests.fetch_add(1);
                    if (!res || res->status != 200) failures.fetch_add(1);
                }
            });
        }
        for (auto& worker : workers) worker.join();
        concurrentRequests = requests.load();
        concurrentFailures = failures.load();
        Check(concurrentRequests == static_cast<std::size_t>(kThreads * kPerThread),
              "8 concurrent clients issued their mixed GET requests");
        Check(concurrentFailures == 0, "all concurrent mixed GET requests returned 200");
        GetJson(port, "/api/v1/health", "", followUpHealth);
        Check(followUpHealth == 200, "gateway is still responsive after the concurrent burst");
    }

    // B8. 未知路径 / 已知路径的错误方法（openapi 兜底错误处理：404 NOT_FOUND）。
    {
        const eda::Json missing = GetJson(port, "/api/v1/not-a-route", token, status);
        unknownPathStatus = status;
        unknownPathCode = ErrorCode(missing);
        Check(status == 404, "unknown path returns 404");
        Check(unknownPathCode == "NOT_FOUND", "unknown path carries NOT_FOUND");
        const eda::Json wrong = PostJson(port, "/api/v1/health", "", eda::Json::object(), status);
        wrongMethodStatus = status;
        wrongMethodCode = ErrorCode(wrong);
        Check(status == 404, "wrong method on a known GET path returns the documented 404");
        Check(wrongMethodCode == "NOT_FOUND" &&
                  Str(At(wrong, "error"), "message") == "status 404",
              "wrong-method rejection uses the documented failure envelope");
    }

    // B9. 事件游标过期：用真实保留窗口（10,000 条）把游标挤出窗口下界。
    // 强制方式：连续 Publish 10,500 条真实事件 → EventStore::TrimLocked 从最旧端裁剪，
    // /state 回显的 oldest_sequence 因此抬升；after < oldest-1 必然 410 CURSOR_EXPIRED。
    {
        trimEvents = 10500;
        for (std::size_t i = 0; i < trimEvents; ++i) {
            server.PublishEvent("agent/state", "prj-expiry", "trace-trim",
                                eda::Json{{"index", static_cast<int>(i)}});
        }
        const eda::Json state =
            GetJson(port, "/api/v1/projects/prj-expiry/state", token, status);
        trimmedOldest = Str(DataOf(state), "oldest_sequence");
        trimmedHighWatermark = Str(DataOf(state), "high_watermark");
        Check(status == 200 && IsDecimalString(At(DataOf(state), "oldest_sequence")),
              "retention trim advanced oldest_sequence to a decimal string");
        const std::uint64_t oldest = trimmedOldest.empty() ? 0 : std::stoull(trimmedOldest);
        Check(oldest > 1, "retention window really dropped the oldest events (oldest_sequence > 1)");
        if (oldest > 1) {
            const eda::Json expired = GetJson(
                port,
                "/api/v1/projects/prj-expiry/events?after=" + std::to_string(oldest - 2) +
                    "&wait_ms=0",
                token, status);
            expiredStatus = status;
            Check(status == 410, "cursor older than the retained window returns 410");
            Check(ErrorCode(expired) == "CURSOR_EXPIRED", "expired cursor carries CURSOR_EXPIRED");
            const eda::Json retained = GetJson(
                port,
                "/api/v1/projects/prj-expiry/events?after=" + std::to_string(oldest - 1) +
                    "&wait_ms=0",
                token, status);
            retainedStatus = status;
            Check(status == 200 && Count(At(DataOf(retained), "events")) > 0,
                  "cursor at the retained window's lower bound still returns events");
        }
    }

    // =====================================================================
    // C. 实测汇总（只打印真正观测到的数字）
    // =====================================================================
    std::cout << "\n[C] NG-10 measured limits / platform\n";
    std::cout << "  platform    : instance_id=" << healthInstanceId
              << " protocol_version=" << healthProtocolVersion << " edition=" << healthEdition
              << "\n";
    std::cout << "  build       : " << healthBuild << "\n";
    std::cout << "  request body: limit=" << config.maxRequestBodyBytes << " bytes; " << underBytes
              << " bytes -> " << underStatus << ", " << overBytes << " bytes -> " << overStatus
              << "\n";
    std::cout << "  response    : limit_bytes=" << responseLimitBytes
              << " actual_bytes=" << responseActualBytes << " status=" << responseCapStatus
              << " (source file " << bigSourceBytes << " bytes, GET /sources/{id})\n";
    std::cout << "  collections : /events default page=" << eventsDefaultPage
              << ", limit=99999 page=" << eventsMaxPage << " (published " << kPageEvents
              << "); /projects/{p}/jobs default page=" << jobsDefaultPage
              << ", limit=99999 page=" << jobsMaxPage << " (submitted " << jobsSubmitted << ")\n";
    std::cout << "  waves       : signals default page=" << waveDefaultPage
              << ", limit=99999 page=" << waveMaxPage << ", total=" << waveTotal << "\n";
    std::cout << "  legacy jobs : limit defaults to " << legacyDefaultLimit << ", clamps to "
              << legacyMaxLimit << " (99999) and " << legacyZeroLimit << " (0)\n";
    std::cout << "  events      : published " << trimEvents
              << " to force retention trim; oldest_sequence=" << trimmedOldest
              << " high_watermark=" << trimmedHighWatermark
              << "; after=oldest-2 -> " << expiredStatus << ", after=oldest-1 -> " << retainedStatus
              << "\n";
    std::cout << "  64-bit      : /state high_watermark=\"" << stateHighWatermark
              << "\"; wave query tick=18446744073709551615 -> status " << waveBoundaryStatus
              << "\n";
    std::cout << "  concurrency : " << concurrentRequests << " GET requests over 8 threads, "
              << concurrentFailures << " failure(s); follow-up /health=" << followUpHealth << "\n";
    std::cout << "  fallback    : unknown path -> " << unknownPathStatus << "/" << unknownPathCode
              << "; POST /health -> " << wrongMethodStatus << "/" << wrongMethodCode << "\n";
    std::cout << "  utf8 path   : manifest bytes == host generic_string() -> " << utf8HostMatches
              << "; gateway loaded UTF-8 non-ASCII source path -> " << utf8Loaded
              << "; host-injected /sources/{id} -> " << utf8SourcesStatus << "\n";

    server.Stop();
    fs::remove_all(root, cleanupError);

    std::cout << (g_failures == 0 ? "ALL PASS" : "FAILURES") << "\n";
    return g_failures == 0 ? 0 : 1;
}
