#include "GatewayServer.h"

#include "GatewayDto.h"
#include "DesignContextService.h"
#include "EventStore.h"
#include "GrantStore.h"
#include "IdempotencyStore.h"
#include "LegacyJobBridge.h"
#include "PathRedaction.h"
#include "ReportNormalizer.h"
#include "SnapshotService.h"

#include <eda/api/build_info.h>
#include "eda-core/Project.h"
#include "eda-platform/Platform.h"
#include "eda-platform/Sha256.h"

#include <httplib.h>

#include <atomic>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <set>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <utility>

namespace eda {
namespace agent {
namespace {

std::string MakeRequestId() {
    static std::atomic<unsigned long> sequence{0};
    return "req-" + std::to_string(sequence.fetch_add(1) + 1);
}

std::string MakeTraceId() {
    static std::atomic<unsigned long> sequence{0};
    return "trace-" + std::to_string(sequence.fetch_add(1) + 1);
}

bool IsAuthorized(const httplib::Request& req, const std::string& token) {
    if (token.empty()) return true;  // 最小脚手架：未配置 token 时放行（正式必须配置）
    const std::string expected = "Bearer " + token;
    return req.get_header_value("Authorization") == expected;
}

std::string OptionalString(const Json& object, const char* key) {
    if (!object.is_object()) return {};
    const auto value = object.find(key);
    return value != object.end() && value->is_string() ? value->get<std::string>()
                                                        : std::string();
}

// SF-03：教育能力参数白名单。没有 provider schema 前，按能力固定允许字段；
// 模型可控的危险字段（可执行文件/工作目录/输出路径/脚本/环境）一律 422 拒绝，
// 不静默剥离——宁可失败也不能把任意路径交给工具。
// sim.run 需要 build_job_id（本计划成功 build 的执行产物登记）。
bool ValidateCapabilityParams(const std::string& capability, const Json& params,
                              std::string& error) {
    error.clear();
    static const std::set<std::string> kForbidden{
        "executable",  "exe",        "binary",     "tool",       "tool_path",
        "command",     "cmd",        "args",       "argv",       "script",
        "script_path", "work_dir",   "working_dir","cwd",        "output",
        "output_path", "out_dir",    "output_dir", "artifact",   "artifact_path",
        "env",         "stdin_file", "source_path"};
    for (auto it = params.begin(); it != params.end(); ++it) {
        if (kForbidden.find(it.key()) != kForbidden.end()) {
            error = "params field '" + it.key() + "' is not allowed";
            return false;
        }
    }
    std::set<std::string> allowed;
    if (capability == "eda.sim.build" || capability == "eda.synth") {
        allowed = {"top_module", "strategy", "opts"};
    } else if (capability == "eda.sim.run") {
        allowed = {"top_module", "testbench", "duration_ticks", "build_job_id"};
    } else {
        error = "capability has no params policy";
        return false;
    }
    for (auto it = params.begin(); it != params.end(); ++it) {
        if (allowed.find(it.key()) == allowed.end()) {
            error = "unknown params field '" + it.key() + "'";
            return false;
        }
        // 简单类型约束。
        const Json& value = it.value();
        if (it.key() == "opts") {
            if (!value.is_array()) {
                error = "opts must be an array of strings";
                return false;
            }
            for (const auto& opt : value) {
                if (!opt.is_string()) {
                    error = "opts must be an array of strings";
                    return false;
                }
            }
        } else if (it.key() == "duration_ticks") {
            if (!value.is_string()) {  // 64 位计数用十进制字符串。
                error = "duration_ticks must be a decimal string";
                return false;
            }
        } else if (!value.is_string()) {
            error = "params field '" + it.key() + "' must be a string";
            return false;
        }
    }
    return true;
}

// Plan contents come from the UI, but they are persisted and later become an
// authorization boundary.  Validate them at issuance time so an accidental or
// malformed UI payload cannot create a grant that is impossible to audit.
bool ValidatePlanSteps(const Json& steps, std::string& error) {
    error.clear();
    if (!steps.is_array()) {
        error = "steps must be an array";
        return false;
    }
    std::set<std::string> seenStepIds;
    for (const Json& step : steps) {
        if (!step.is_object()) {
            error = "each plan step must be an object";
            return false;
        }
        const std::string stepId = OptionalString(step, "step_id");
        const std::string capability = OptionalString(step, "capability");
        if (stepId.empty() || capability.empty() || !seenStepIds.insert(stepId).second) {
            error = "each plan step needs a unique step_id and capability";
            return false;
        }
        const Json params = step.value("params", Json::object());
        if (!params.is_object() || !ValidateCapabilityParams(capability, params, error)) return false;
        if (step.contains("depends_on_job_ids")) {
            if (!step["depends_on_job_ids"].is_array()) {
                error = "depends_on_job_ids must be an array";
                return false;
            }
            for (const Json& jobId : step["depends_on_job_ids"]) {
                if (!jobId.is_string() || jobId.get<std::string>().empty()) {
                    error = "depends_on_job_ids must contain non-empty job ids";
                    return false;
                }
            }
        }
    }
    return true;
}

// A populated grant.steps turns a quota-only legacy grant into a concrete,
// ordered plan.  Old grants with no steps remain readable for compatibility,
// but cannot claim plan-level enforcement until re-issued with steps.
bool ValidateGrantStep(const Grant& grant, const Json& request,
                       const std::string& capability, const Json& params,
                       IJobService* service, const std::string& projectId,
                       const std::string& snapshotId,
                       const std::unordered_map<std::string, Json>& jobBindings,
                       std::string& error) {
    error.clear();
    if (!grant.steps.is_array() || grant.steps.empty()) return true;
    const std::string planHash = OptionalString(request, "plan_hash");
    const std::string stepId = OptionalString(request, "step_id");
    if (planHash.empty() || planHash != grant.planHash || stepId.empty()) {
        error = "a stepped grant requires matching plan_hash and step_id";
        return false;
    }
    const Json* step = nullptr;
    for (const Json& candidate : grant.steps) {
        if (candidate.is_object() && candidate.value("step_id", std::string()) == stepId) {
            step = &candidate;
            break;
        }
    }
    if (step == nullptr || step->value("capability", std::string()) != capability) {
        error = "step_id is not authorized for the requested capability";
        return false;
    }
    if (step->contains("params") && (*step)["params"].is_object()) {
        if (params.size() != (*step)["params"].size()) {
            error = "requested params do not exactly match the approved plan step";
            return false;
        }
        for (auto allowed = (*step)["params"].begin(); allowed != (*step)["params"].end();
             ++allowed) {
            const auto supplied = params.find(allowed.key());
            if (supplied == params.end() || *supplied != allowed.value()) {
                error = "requested params do not match the approved plan step";
                return false;
            }
        }
    }
    if (!step->contains("depends_on_job_ids")) return true;
    if (!(*step)["depends_on_job_ids"].is_array() || service == nullptr) {
        error = "approved plan step has invalid dependencies";
        return false;
    }
    for (const Json& required : (*step)["depends_on_job_ids"]) {
        if (!required.is_string()) {
            error = "approved plan dependency is invalid";
            return false;
        }
        const std::string jobId = required.get<std::string>();
        const std::optional<JobRecord> record = service->get(jobId);
        if (!record.has_value() || record->state != JobState::Succeeded) {
            error = "approved plan dependency is not a succeeded job";
            return false;
        }
        const auto binding = jobBindings.find(jobId);
        if (binding == jobBindings.end() ||
            binding->second.value("project_id", std::string()) != projectId ||
            binding->second.value("snapshot_id", std::string()) != snapshotId) {
            error = "approved plan dependency belongs to another project or snapshot";
            return false;
        }
    }
    return true;
}

// Agent 只能提供受限的“意图参数”。真正交给官方插件的文件、输出和可执行文件
// 必须由已校验的 snapshot / 已完成 build Job 推导，绝不能回传 Agent 给出的路径。
bool BuildTrustedJobParams(const std::string& capability, const Json& requested,
                           const SnapshotRecord& snapshot,
                           const SnapshotService& snapshots, IJobService* service,
                           Json& trusted, std::string& error) {
    trusted = Json::object();
    error.clear();
    const std::string requestedTop = OptionalString(requested, "top_module");
    if (snapshot.top.empty()) {
        error = "snapshot has no top module";
        return false;
    }
    if (!requestedTop.empty() && requestedTop != snapshot.top) {
        error = "top_module must match the approved snapshot";
        return false;
    }
    trusted["top_module"] = snapshot.top;

    if (capability == "eda.synth" || capability == "eda.sim.build") {
        const std::filesystem::path fileRoot = snapshots.SnapshotDirectory(snapshot.id) / "files";
        Json sourceFiles = Json::array();
        for (const SnapshotSource& source : snapshot.sources) {
            // Lookup() has already checked relativePath, containment and hash before this point.
            sourceFiles.push_back(platform::PathToUtf8(fileRoot / source.relativePath));
        }
        if (sourceFiles.empty()) {
            error = "snapshot has no runnable source files";
            return false;
        }
        trusted["source_files"] = std::move(sourceFiles);
    }

    if (capability == "eda.synth") {
        const std::string strategy = OptionalString(requested, "strategy");
        trusted["strategy"] = strategy.empty() ? "baseline" : strategy;
        return true;
    }

    if (capability == "eda.sim.build") {
        const std::string testbench = OptionalString(requested, "testbench");
        if (!testbench.empty()) {
            const auto match = std::find_if(snapshot.sources.begin(), snapshot.sources.end(),
                [&testbench](const SnapshotSource& source) { return source.relativePath == testbench; });
            if (match == snapshot.sources.end()) {
                error = "testbench must be a source in the approved snapshot";
                return false;
            }
            trusted["testbench"] = platform::PathToUtf8(
                snapshots.SnapshotDirectory(snapshot.id) / "files" / match->relativePath);
        }
        return true;
    }

    if (capability == "eda.sim.run") {
        if (service == nullptr) {
            error = "job service is unavailable";
            return false;
        }
        const std::string buildJobId = OptionalString(requested, "build_job_id");
        const JobReport buildReport = service->report(buildJobId);
        const auto executable = std::find_if(buildReport.artifacts.begin(), buildReport.artifacts.end(),
            [](const Artifact& artifact) { return artifact.id == "sim-executable"; });
        std::error_code ec;
        if (executable == buildReport.artifacts.end() || executable->path.empty() ||
            !std::filesystem::is_regular_file(executable->path, ec) || ec) {
            error = "succeeded sim.build has no available executable artifact";
            return false;
        }
        trusted["sim_exe"] = platform::PathToUtf8(executable->path);
        return true;
    }

    error = "capability has no trusted execution mapping";
    return false;
}

std::string GatewayArtifactId(const std::string& jobId, const Artifact& artifact) {
    // Artifact ids coming from plugins are often role names such as
    // "sim-executable" or "top.json", and therefore are not globally unique.
    // Publish a deterministic, opaque id scoped by the job instead.
    const std::string material = jobId + "\n" + artifact.id + "\n" +
                                 platform::PathToUtf8(artifact.path);
    return "art-" + platform::Sha256Hex(material.data(), material.size()).substr(0, 24);
}

bool IsWaveArtifact(const Artifact& artifact) {
    std::string extension = artifact.path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return extension == ".vcd" || extension == ".fst" || extension == ".lxt" ||
           extension == ".lxt2";
}

// UI 专用身份（grant/receipt 签发、撤销）。Agent token 不得通过。
bool IsUiAuthorized(const httplib::Request& req, const GatewayConfig& config) {
    if (config.uiToken.empty()) return false;  // 未配置 UI token 时，签发一律拒绝
    return req.get_header_value("Authorization") == ("Bearer " + config.uiToken);
}

// 允许的 Host 头：loopback 地址（含端口）。
bool IsLoopbackHost(const std::string& host) {
    if (host.empty()) return false;
    const std::string value = host;
    const char* prefixes[] = {"127.0.0.1", "localhost", "[::1]"};
    for (const char* prefix : prefixes) {
        const std::size_t length = std::strlen(prefix);
        if (value.size() >= length && value.compare(0, length, prefix) == 0) {
            // 之后必须是空（无端口）或 ':'（端口）。
            if (value.size() == length || value[length] == ':') return true;
        }
    }
    return false;
}

// Origin 校验：无 Origin（本机直连/命令行工具）放行；有 Origin 必须为 loopback。
bool IsAllowedOrigin(const std::string& origin) {
    if (origin.empty()) return true;
    const std::string lower = [&origin]() {
        std::string value = origin;
        for (char& c : value) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return value;
    }();
    const char* prefixes[] = {"http://127.0.0.1", "https://127.0.0.1", "http://localhost",
                              "https://localhost", "http://[::1]", "https://[::1]"};
    for (const char* prefix : prefixes) {
        const std::size_t length = std::strlen(prefix);
        if (lower.size() >= length && lower.compare(0, length, prefix) == 0) {
            if (lower.size() == length || lower[length] == ':') return true;
        }
    }
    return false;
}

// 安全边界守卫：Host / Origin。返回 false 时已写入错误应答。
bool PassesBoundary(const httplib::Request& req, const GatewayConfig& config,
                    httplib::Response& res,
                    const std::function<void(httplib::Response&, const Json&)>& send) {
    if (config.enforceHostHeader && !IsLoopbackHost(req.get_header_value("Host"))) {
        res.status = 403;
        send(res, FailureEnvelope(MakeRequestId(), MakeTraceId(), "POLICY_DENIED",
                                  "unexpected Host header", false));
        return false;
    }
    if (config.enforceOrigin && !IsAllowedOrigin(req.get_header_value("Origin"))) {
        res.status = 403;
        send(res, FailureEnvelope(MakeRequestId(), MakeTraceId(), "POLICY_DENIED",
                                  "unexpected Origin", false));
        return false;
    }
    return true;
}

bool IsWithinPath(const std::filesystem::path& child, const std::filesystem::path& root) {
    const std::filesystem::path relative = child.lexically_relative(root);
    if (relative.empty() || relative.is_absolute()) return child == root;
    const auto first = relative.begin();
    return first == relative.end() || *first != "..";
}

bool ParseDecimal(const Json& value, std::uint64_t& number) {
    std::string text;
    if (value.is_string()) {
        text = value.get<std::string>();
    } else if (value.is_number_unsigned()) {
        number = value.get<std::uint64_t>();
        return true;
    } else {
        return false;
    }
    if (text.empty()) return false;
    std::uint64_t parsed = 0;
    for (const char c : text) {
        if (c < '0' || c > '9') return false;
        const std::uint64_t digit = static_cast<std::uint64_t>(c - '0');
        if (parsed > (std::numeric_limits<std::uint64_t>::max() - digit) / 10) return false;
        parsed = parsed * 10 + digit;
    }
    number = parsed;
    return true;
}

bool WriteJsonReplace(const std::filesystem::path& path, const Json& document,
                      std::string& error) {
    error.clear();
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
        error = "unable to create revision directory: " + ec.message();
        return false;
    }
    static std::atomic<unsigned long long> sequence{0};
    const auto ticks = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    const std::filesystem::path temporary =
        path.parent_path() /
        (path.filename().string() + ".tmp-" + std::to_string(ticks) + "-" +
         std::to_string(sequence.fetch_add(1)));
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) {
            error = "unable to open temporary revision file";
            return false;
        }
        output << document.dump(2) << '\n';
        output.flush();
        if (!output) {
            error = "unable to write temporary revision file";
            std::filesystem::remove(temporary, ec);
            return false;
        }
    }
    ec.clear();
    std::filesystem::rename(temporary, path, ec);
    if (!ec) return true;

    // std::filesystem::rename cannot replace an existing file on Windows. Preserve the
    // old index until the new file has been fully written, and restore it if replacement fails.
    const std::filesystem::path backup =
        path.parent_path() /
        (path.filename().string() + ".bak-" + std::to_string(ticks) + "-" +
         std::to_string(sequence.fetch_add(1)));
    std::error_code backupError;
    std::filesystem::rename(path, backup, backupError);
    if (backupError) {
        error = "unable to preserve revision file: " + backupError.message();
        std::filesystem::remove(temporary, ec);
        return false;
    }
    ec.clear();
    std::filesystem::rename(temporary, path, ec);
    if (ec) {
        std::error_code restoreError;
        std::filesystem::rename(backup, path, restoreError);
        error = "unable to replace revision file: " + ec.message();
        std::filesystem::remove(temporary, restoreError);
        return false;
    }
    std::filesystem::remove(backup, backupError);
    return true;
}

bool ObserveRevision(const std::filesystem::path& projectRoot, const std::string& projectId,
                     const std::string& fingerprint, std::string& revision,
                     std::string& error) {
    const std::filesystem::path revisionPath =
        projectRoot / ".sigflow" / "agent" / "revision.json";
    Json index;
    std::uint64_t number = 0;
    std::string priorFingerprint;
    std::error_code ec;
    const bool exists = std::filesystem::exists(revisionPath, ec);
    if (ec) {
        error = "unable to inspect revision index: " + ec.message();
        return false;
    }
    if (exists) {
        std::ifstream input(revisionPath, std::ios::binary);
        if (!input) {
            error = "unable to read revision index";
            return false;
        }
        try {
            input >> index;
        } catch (const std::exception& exception) {
            error = std::string("invalid revision index: ") + exception.what();
            return false;
        }
        if (!index.is_object() || index.value("schema", std::string()) != "edu.revision.v1" ||
            index.value("project_id", std::string()) != projectId ||
            !ParseDecimal(index["sequence"], number) || number == 0 ||
            !index.contains("design_fingerprint") || !index["design_fingerprint"].is_string() ||
            !index.contains("revision") || !index["revision"].is_string()) {
            error = "revision index has an invalid schema or project identity";
            return false;
        }
        priorFingerprint = index["design_fingerprint"].get<std::string>();
        if (priorFingerprint == fingerprint) {
            revision = index["revision"].get<std::string>();
            if (revision.empty()) {
                error = "revision index contains an empty revision";
                return false;
            }
            return true;
        }
        if (number == std::numeric_limits<std::uint64_t>::max()) {
            error = "revision sequence is exhausted";
            return false;
        }
        ++number;
    } else {
        number = 1;
    }

    revision = "rev-" + std::to_string(number) + "-" + fingerprint.substr(0, 12);
    Json next{{"schema", "edu.revision.v1"},
              {"project_id", projectId},
              {"sequence", std::to_string(number)},
              {"design_fingerprint", fingerprint},
              {"revision", revision},
              {"updated_at", platform::UtcTimestamp()}};
    return WriteJsonReplace(revisionPath, next, error);
}

} // namespace

struct GatewayServer::Impl {
    GatewayConfig config;
    ReadyPluginProvider provider;
    JobServiceProvider jobServiceProvider;
    std::unique_ptr<httplib::Server> server;
    std::thread worker;
    std::atomic<bool> running{false};
    int boundPort = 0;
    EventStore events{10000};
    GrantStore grants;
    IdempotencyStore idempotency;
    ReceiptStore receipts;
    WaveformService waveformService;
    ArtifactService artifactService;
    // SF-02：Job 生命周期事件源（宿主经 PollJobEvents 驱动）。
    JobEventSource jobEvents;
    // NG-05：局部设计证据与未保存缓冲边界（宿主写、HTTP worker 只读）。
    DesignContextService designContext;
    std::mutex projectStateMutex;
    std::unordered_map<std::string, ProjectSnapshotState> projectStates;
    // Job 归属：job_id -> project_id（用于 GET /jobs 与取消归属校验）。
    std::mutex jobMutex;
    std::unordered_map<std::string, std::string> jobProjects;
    // SF-03：Job 绑定（提交时登记）：job_id -> project/snapshot/revision/capability/jobType。
    std::unordered_map<std::string, Json> jobBindings;
    // NG-08：可信事件源的去重状态（同一事实只投递一次）。
    std::mutex eventSourceMutex;
    std::unordered_map<std::string, std::string> lastSavedRevision;   // project_id -> revision
    std::unordered_map<std::string, std::string> lastSelectionState;  // project_id -> fingerprint
    std::unordered_map<std::string, bool> lastDirtyState;             // project_id -> dirty
    std::chrono::steady_clock::time_point lastRetentionRun{};
    bool retentionRanOnce = false;
};

GatewayServer::GatewayServer(GatewayConfig config, ReadyPluginProvider provider)
    : impl_(std::make_unique<Impl>()) {
    impl_->config = std::move(config);
    impl_->provider = std::move(provider);
}

GatewayServer::~GatewayServer() { Stop(); }

void GatewayServer::SetJobServiceProvider(JobServiceProvider provider) {
    if (impl_) impl_->jobServiceProvider = std::move(provider);
}

void GatewayServer::SetWaveformBackendFactory(WaveformBackendFactory factory) {
    if (impl_) impl_->waveformService.SetBackendFactory(std::move(factory));
}

void GatewayServer::RegisterWaveArtifact(const WaveArtifactRecord& record) {
    if (impl_) impl_->waveformService.Register(record);
}

void GatewayServer::RemoveWaveProject(const std::string& projectId) {
    if (impl_) impl_->waveformService.RemoveProject(projectId);
}

void GatewayServer::RegisterArtifact(const ArtifactRecord& record) {
    if (!impl_) return;
    impl_->artifactService.Register(record);
    // NG-08：产物产生是可信事件源（由宿主在 Job 产物完成后登记时触发）。
    // 事件只带 opaque artifact_id 与 hash，绝不含本机路径。
    if (record.projectId.empty()) return;
    Json data{{"project_id", record.projectId}, {"artifact_id", record.artifactId}};
    if (!record.jobId.empty()) data["job_id"] = record.jobId;
    if (!record.revision.empty()) data["revision"] = record.revision;
    if (!record.schema.empty()) data["schema"] = record.schema;
    if (!record.role.empty()) data["role"] = record.role;
    if (!record.sha256.empty()) data["sha256"] = record.sha256;
    PublishEvent("artifact/produced", record.projectId, MakeTraceId(), data);
}

void GatewayServer::UpdateSelectionContext(const std::string& projectId,
                                           const SelectionContext& selection) {
    if (!impl_ || projectId.empty()) return;
    impl_->designContext.SetSelection(selection, projectId);
    // NG-08：选择变化事件。按 (revision, state_version, buffer_hash, 选中节点集合) 去重，
    // 保证同一选择重复上报不会刷屏。
    std::string fingerprint = selection.revision + "|" + std::to_string(selection.stateVersion) +
                              "|" + selection.bufferHash + "|" + (selection.dirty ? "1" : "0");
    for (const std::string& nodeId : selection.selectedNodeIds) fingerprint += "|" + nodeId;
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(impl_->eventSourceMutex);
        const auto it = impl_->lastSelectionState.find(projectId);
        changed = it == impl_->lastSelectionState.end() || it->second != fingerprint;
        impl_->lastSelectionState[projectId] = fingerprint;
    }
    if (changed) {
        PublishEvent("selection/changed", projectId, MakeTraceId(),
                     Json{{"project_id", projectId},
                          {"revision", selection.revision},
                          {"state_version", std::to_string(selection.stateVersion)},
                          {"selected_node_ids", selection.selectedNodeIds},
                          {"dirty", selection.dirty},
                          {"has_unsaved_buffer", !selection.bufferExcerpts.empty()}});
    }
}

void GatewayServer::SetDesignNodes(const std::string& projectId, const std::string& revision,
                                   std::vector<DesignNodeRecord> nodes) {
    if (impl_) impl_->designContext.SetNodes(projectId, revision, std::move(nodes));
}

void GatewayServer::ClearDesignContext(const std::string& projectId) {
    if (impl_) impl_->designContext.ClearProject(projectId);
}

bool GatewayServer::PruneProjectSnapshots(const std::string& projectId, std::size_t keepRecent,
                                          std::uint64_t maxAgeSeconds, std::size_t& removed,
                                          std::string& error, std::uint64_t nowEpoch) {
    removed = 0;
    error.clear();
    if (!impl_) {
        error = "gateway is unavailable";
        return false;
    }
    std::filesystem::path projectRoot;
    std::string currentRevision;
    std::set<std::string> protectedIds;
    {
        std::lock_guard<std::mutex> lock(impl_->projectStateMutex);
        const auto it = impl_->projectStates.find(projectId);
        if (it != impl_->projectStates.end()) {
            projectRoot = it->second.request.projectRoot;
            currentRevision = it->second.request.currentRevision;
        }
    }
    if (projectRoot.empty()) {
        error = "unknown project";
        return false;
    }
    // 活跃引用集合：agent Job 绑定 + active grant + 当前 revision。
    {
        std::lock_guard<std::mutex> lock(impl_->jobMutex);
        for (const auto& binding : impl_->jobBindings) {
            const Json& b = binding.second;
            if (b.value("project_id", std::string()) != projectId) continue;
            const std::string snapshotId = b.value("snapshot_id", std::string());
            if (!snapshotId.empty()) protectedIds.insert(snapshotId);
        }
    }
    {
        // Grant 引用（store 自带锁）。
        const auto activeGrantIds = impl_->grants.ActiveGrantIds(projectId);
        for (const auto& grantId : activeGrantIds) {
            Grant grant;
            if (!impl_->grants.Lookup(grantId, grant)) continue;
            if (grant.snapshotId.empty()) continue;
            if (grant.status != "active") continue;
            protectedIds.insert(grant.snapshotId);
        }
    }
    // 列出该项目的快照，把当前 revision 的全部快照视为受保护。
    SnapshotService snapshotService(projectRoot / ".sigflow" / "agent" / "snapshots");
    std::vector<SnapshotRecord> all;
    if (!snapshotService.List(projectId, all, error)) return false;
    for (const auto& record : all) {
        if (record.revision == currentRevision) protectedIds.insert(record.id);
    }
    return snapshotService.Prune(projectId, protectedIds, keepRecent, maxAgeSeconds, removed,
                                 error, nowEpoch);
}

bool GatewayServer::Start(std::string& error) {
    if (impl_->running.load()) {
        error = "gateway already running";
        return false;
    }
    if (!impl_->config.grantStorePath.empty() &&
        !impl_->grants.ConfigurePersistence(impl_->config.grantStorePath, error)) {
        return false;
    }
    impl_->server = std::make_unique<httplib::Server>();
    impl_->server->set_read_timeout(25, 0);
    impl_->server->set_write_timeout(25, 0);
    // SF-02：请求体限额（超限返回 413，由 error_handler 统一信封）。
    impl_->server->set_payload_max_length(impl_->config.maxRequestBodyBytes);
    // SF-02：重放已落盘事件（重启恢复）。
    if (!impl_->config.eventLogPath.empty()) {
        impl_->events.LoadFromFile(impl_->config.eventLogPath);
    }

    const GatewayConfig config = impl_->config;
    ReadyPluginProvider provider = impl_->provider;

    const auto send = [maxResponse = impl_->config.maxResponseBodyBytes](httplib::Response& res,
                                                                        const Json& body) {
        std::string payload = body.dump();
        // SF-02：普通响应 2 MiB 限额。超限时不静默截断（会破坏 JSON），
        // 改为返回可机器判定的 RESOURCE_EXHAUSTED，并提示改用分页/受限读取路由。
        if (maxResponse > 0 && payload.size() > maxResponse) {
            Json failure = FailureEnvelope(MakeRequestId(), MakeTraceId(), "RESOURCE_EXHAUSTED",
                                           "response exceeds the configured size limit", false);
            failure["error"]["details"] =
                Json{{"limit_bytes", std::to_string(maxResponse)},
                     {"actual_bytes", std::to_string(payload.size())}};
            payload = failure.dump();
            res.status = 429;
        }
        res.set_content(payload, "application/json; charset=utf-8");
    };

    impl_->server->Get("/api/v1/health", [config, send](const httplib::Request& req,
                                                        httplib::Response& res) {
        if (!PassesBoundary(req, config, res, send)) return;
        const Json envelope = SuccessEnvelope(
            MakeRequestId(), MakeTraceId(),
            BuildHealthData(config.instanceId, config.protocolVersion, config.edition,
                            config.build.empty() ? BuildInfo() : config.build));
        res.status = 200;
        send(res, envelope);
    });

    impl_->server->Get("/api/v1/capabilities",
                       [config, provider, send](const httplib::Request& req,
                                                httplib::Response& res) {
        if (!PassesBoundary(req, config, res, send)) return;
        if (!IsAuthorized(req, config.token)) {
            res.status = 401;
            send(res, FailureEnvelope(MakeRequestId(), MakeTraceId(), "UNAUTHENTICATED",
                                      "missing or invalid bearer token", false));
            return;
        }
        const std::vector<ReadyPlugin> ready = provider ? provider() : std::vector<ReadyPlugin>{};
        const std::vector<CapabilityStatus> capabilities = ResolveEducationCapabilities(ready);
        const Json envelope = SuccessEnvelope(
            MakeRequestId(), MakeTraceId(),
            BuildCapabilitiesData(config.instanceId, config.protocolVersion, config.edition,
                                  config.build.empty() ? BuildInfo() : config.build,
                                  capabilities));
        res.status = 200;
        send(res, envelope);
    });

    // SF-05：GET /projects/{p}/context —— UI 线程预先生成的不可变工程 DTO。
    impl_->server->Get(R"(/api/v1/projects/([^/]+)/context)",
                       [this, config, send](const httplib::Request& req,
                                            httplib::Response& res) {
        if (!PassesBoundary(req, config, res, send)) return;
        if (!IsAuthorized(req, config.token)) {
            res.status = 401;
            send(res, FailureEnvelope(MakeRequestId(), MakeTraceId(), "UNAUTHENTICATED",
                                      "missing or invalid bearer token", false));
            return;
        }
        const std::string projectId = req.matches.size() > 1 ? req.matches[1].str() : "";
        ProjectSnapshotState state;
        {
            std::lock_guard<std::mutex> lock(impl_->projectStateMutex);
            const auto it = impl_->projectStates.find(projectId);
            if (it == impl_->projectStates.end()) {
                res.status = 404;
                send(res, FailureEnvelope(MakeRequestId(), MakeTraceId(), "NOT_FOUND",
                                          "unknown project", false));
                return;
            }
            state = it->second;
        }
        Json data{{"project_id", projectId},
                  {"revision", state.request.currentRevision},
                  {"dirty", state.request.dirty},
                  {"synchronized", state.request.synchronized},
                  {"top", state.request.top},
                  {"target", state.request.target},
                  {"sources", state.sourceSummary},
                  {"buffer_hash", state.bufferHash.empty()
                                      ? Json(nullptr)
                                      : Json(state.bufferHash)},
                  {"policy_version", state.policyVersion.empty()
                                         ? Json(nullptr)
                                         : Json(state.policyVersion)}};
        res.status = 200;
        send(res, SuccessEnvelope(MakeRequestId(), MakeTraceId(), data));
    });

    // SF-05：POST /projects/{p}/snapshots —— 仅已保存、同步完成且 revision 命中。
    impl_->server->Post(R"(/api/v1/projects/([^/]+)/snapshots)",
                        [this, config, send](const httplib::Request& req,
                                             httplib::Response& res) {
        if (!PassesBoundary(req, config, res, send)) return;
        if (!IsAuthorized(req, config.token)) {
            res.status = 401;
            send(res, FailureEnvelope(MakeRequestId(), MakeTraceId(), "UNAUTHENTICATED",
                                      "missing or invalid bearer token", false));
            return;
        }
        const std::string projectId = req.matches.size() > 1 ? req.matches[1].str() : "";
        Json body;
        try {
            body = req.body.empty() ? Json::object() : Json::parse(req.body);
        } catch (const std::exception&) {
            res.status = 400;
            send(res, FailureEnvelope(MakeRequestId(), MakeTraceId(), "INVALID_ARGUMENT",
                                      "request body is not valid JSON", false));
            return;
        }
        const std::string expectedRevision = body.contains("expected_revision") &&
                                                     body["expected_revision"].is_string()
                                                 ? body["expected_revision"].get<std::string>()
                                                 : std::string();
        const bool validRequireSaved = !body.contains("require_saved") ||
                                       body["require_saved"].is_boolean();
        const bool requireSaved = !body.contains("require_saved") ||
                                  (validRequireSaved && body["require_saved"].get<bool>());
        if (expectedRevision.empty() || !validRequireSaved || !requireSaved) {
            res.status = 400;
            send(res, FailureEnvelope(MakeRequestId(), MakeTraceId(), "INVALID_ARGUMENT",
                                      "expected_revision and require_saved=true are required", false));
            return;
        }

        ProjectSnapshotState state;
        {
            std::lock_guard<std::mutex> lock(impl_->projectStateMutex);
            const auto it = impl_->projectStates.find(projectId);
            if (it == impl_->projectStates.end()) {
                res.status = 404;
                send(res, FailureEnvelope(MakeRequestId(), MakeTraceId(), "NOT_FOUND",
                                          "unknown project", false));
                return;
            }
            state = it->second;
        }
        state.request.expectedRevision = expectedRevision;
        SnapshotService service(state.request.projectRoot / ".sigflow" / "agent" / "snapshots");
        const SnapshotResult result = service.Create(state.request, [this, projectId]() {
            std::lock_guard<std::mutex> lock(impl_->projectStateMutex);
            const auto it = impl_->projectStates.find(projectId);
            if (it == impl_->projectStates.end() || it->second.request.dirty ||
                !it->second.request.synchronized) {
                return std::string();
            }
            return it->second.request.currentRevision;
        });
        if (!result) {
            std::string code = SnapshotErrorCode(result.code);
            int status = 422;
            if (result.code == SnapshotError::StaleRevision ||
                result.code == SnapshotError::DirtyProject ||
                result.code == SnapshotError::SyncInProgress) {
                status = 409;
                if (result.code != SnapshotError::StaleRevision) code = "STALE_REVISION";
            } else if (result.code == SnapshotError::InvalidArgument ||
                       result.code == SnapshotError::PathOutsideProject) {
                status = 400;
            } else if (result.code == SnapshotError::StorageError) {
                status = 503;
                code = "SERVICE_UNAVAILABLE";
            }
            res.status = status;
            send(res, FailureEnvelope(MakeRequestId(), MakeTraceId(), code,
                                      result.message, status == 503));
            return;
        }
        const Json data = SnapshotService::ToJson(result.snapshot);
        const Json event = impl_->events.Append("snapshot/created", projectId,
                                                MakeTraceId(), data);
        if (!config.eventLogPath.empty()) {
            impl_->events.AppendToFile(config.eventLogPath, event);
        }
        res.status = 201;
        send(res, SuccessEnvelope(MakeRequestId(), MakeTraceId(), data));
    });

    // SF-07：GET /projects/{p}/sources/{source_id} —— 有界行区间的源码 + SourceRef。
    // 只读已登记源文件；读取内容必须与 DTO 记录的 hash 一致（防 IDE 外部改写窗口）。
    impl_->server->Get(R"(/api/v1/projects/([^/]+)/sources/([^/]+))",
                       [this, config, send](const httplib::Request& req, httplib::Response& res) {
        if (!PassesBoundary(req, config, res, send)) return;
        if (!IsAuthorized(req, config.token)) {
            res.status = 401;
            send(res, FailureEnvelope(MakeRequestId(), MakeTraceId(), "UNAUTHENTICATED",
                                      "missing or invalid bearer token", false));
            return;
        }
        const std::string projectId = req.matches.size() > 1 ? req.matches[1].str() : "";
        const std::string sourceId = req.matches.size() > 2 ? req.matches[2].str() : "";
        ProjectSnapshotState state;
        {
            std::lock_guard<std::mutex> lock(impl_->projectStateMutex);
            const auto it = impl_->projectStates.find(projectId);
            if (it == impl_->projectStates.end()) {
                res.status = 404;
                send(res, FailureEnvelope(MakeRequestId(), MakeTraceId(), "NOT_FOUND",
                                          "unknown project", false));
                return;
            }
            state = it->second;
        }
        // 定位登记的源摘要。
        Json summary;
        for (const auto& entry : state.sourceSummary) {
            if (entry.value("source_id", std::string()) == sourceId) {
                summary = entry;
                break;
            }
        }
        if (summary.is_null() || !summary.is_object()) {
            res.status = 404;
            send(res, FailureEnvelope(MakeRequestId(), MakeTraceId(), "NOT_FOUND",
                                      "unknown source id", false));
            return;
        }
        // 行区间（1 基、包含端点；缺省全文）。
        const std::string relativePath = summary.value("path", std::string());
        std::uint64_t startLine = 1;
        std::uint64_t endLine = 0;  // 0 = 到文件末尾
        if (req.has_param("start_line")) {
            try { startLine = std::stoull(req.get_param_value("start_line")); } catch (...) { startLine = 1; }
        }
        if (req.has_param("end_line")) {
            try { endLine = std::stoull(req.get_param_value("end_line")); } catch (...) { endLine = 0; }
        }
        const std::filesystem::path absolute =
            state.request.projectRoot / platform::PathFromUtf8(relativePath);
        const std::string currentHash = platform::Sha256FileHex(absolute);
        if (currentHash.empty()) {
            res.status = 410;
            send(res, FailureEnvelope(MakeRequestId(), MakeTraceId(), "ARTIFACT_EXPIRED",
                                      "source is no longer available", false));
            return;
        }
        if (currentHash != summary.value("sha256", std::string())) {
            res.status = 409;
            send(res, FailureEnvelope(MakeRequestId(), MakeTraceId(), "STALE_REVISION",
                                      "source changed since context was built", false));
            return;
        }
        std::ifstream input(absolute, std::ios::binary);
        if (!input) {
            res.status = 410;
            send(res, FailureEnvelope(MakeRequestId(), MakeTraceId(), "ARTIFACT_EXPIRED",
                                      "source cannot be read", false));
            return;
        }
        std::string line;
        std::string text;
        std::uint64_t lineNumber = 0;
        std::uint64_t lastLine = 0;
        while (std::getline(input, line)) {
            ++lineNumber;
            if (lineNumber < startLine) continue;
            if (endLine != 0 && lineNumber > endLine) break;
            text += line;
            text += '\n';
            lastLine = lineNumber;
        }
        Json data;
        data["project_id"] = projectId;
        data["revision"] = state.request.currentRevision;
        data["source_id"] = sourceId;
        data["file_hash"] = summary.value("sha256", std::string());
        data["path"] = relativePath;
        data["start_line"] = startLine;
        data["end_line"] = lastLine;
        data["text"] = text;
        data["source_ref"] = Json{{"project_id", projectId},
                                  {"revision", state.request.currentRevision},
                                  {"source_id", sourceId},
                                  {"file_hash", summary.value("sha256", std::string())},
                                  {"start_line", startLine},
                                  {"end_line", lastLine},
                                  {"node_id", nullptr}};
        res.status = 200;
        send(res, SuccessEnvelope(MakeRequestId(), MakeTraceId(), data));
    });

    // NG-05：GET /projects/{p}/design/nodes/{node_id} —— 只读局部设计证据。
    // 语义：available / ambiguous / stale / unavailable。无法可靠映射时返回 stale 或
    // unavailable，不猜测高亮对象、不伪造行号。未保存缓冲只影响 unsaved_only 标记；
    // 设计上下文永远 usable_for_execution=false，只有 SigFlow 签发的 snapshot 才绑定执行。
    impl_->server->Get(R"(/api/v1/projects/([^/]+)/design/nodes/([^/]+))",
                       [this, config, send](const httplib::Request& req, httplib::Response& res) {
        if (!PassesBoundary(req, config, res, send)) return;
        if (!IsAuthorized(req, config.token)) {
            res.status = 401;
            send(res, FailureEnvelope(MakeRequestId(), MakeTraceId(), "UNAUTHENTICATED",
                                      "missing or invalid bearer token", false));
            return;
        }
        const std::string projectId = req.matches.size() > 1 ? req.matches[1].str() : "";
        const std::string nodeId = req.matches.size() > 2 ? req.matches[2].str() : "";
        if (nodeId.empty()) {
            res.status = 400;
            send(res, FailureEnvelope(MakeRequestId(), MakeTraceId(), "INVALID_ARGUMENT",
                                      "node_id is required", false));
            return;
        }
        ProjectSnapshotState state;
        {
            std::lock_guard<std::mutex> lock(impl_->projectStateMutex);
            const auto it = impl_->projectStates.find(projectId);
            if (it == impl_->projectStates.end()) {
                res.status = 404;
                send(res, FailureEnvelope(MakeRequestId(), MakeTraceId(), "NOT_FOUND",
                                          "unknown project", false));
                return;
            }
            state = it->second;
        }
        // 显式 revision 必须与当前一致：旧卡不得定位到新版本图元。
        if (req.has_param("revision")) {
            const std::string requested = req.get_param_value("revision");
            if (!requested.empty() && requested != state.request.currentRevision) {
                res.status = 409;
                send(res, FailureEnvelope(MakeRequestId(), MakeTraceId(), "STALE_REVISION",
                                          "requested revision is not the current revision",
                                          false));
                return;
            }
        }
        const DesignContextService::NodeResolution resolution = impl_->designContext.ResolveNode(
            projectId, nodeId, state.request.currentRevision, state.request.projectRoot);
        res.status = 200;
        send(res, SuccessEnvelope(MakeRequestId(), MakeTraceId(),
                                  DesignContextService::NodeToJson(
                                      projectId, state.request.currentRevision,
                                      state.request.dirty, resolution)));
    });

    // SF-07 / NG-05：POST /projects/{p}/context/query —— 按需裁剪的证据包（max_bytes + omitted/reason）。
    impl_->server->Post(R"(/api/v1/projects/([^/]+)/context/query)",
                        [this, config, send](const httplib::Request& req, httplib::Response& res) {
        if (!PassesBoundary(req, config, res, send)) return;
        if (!IsAuthorized(req, config.token)) {
            res.status = 401;
            send(res, FailureEnvelope(MakeRequestId(), MakeTraceId(), "UNAUTHENTICATED",
                                      "missing or invalid bearer token", false));
            return;
        }
        const std::string projectId = req.matches.size() > 1 ? req.matches[1].str() : "";
        Json body;
        try {
            body = req.body.empty() ? Json::object() : Json::parse(req.body);
        } catch (const std::exception&) {
            res.status = 400;
            send(res, FailureEnvelope(MakeRequestId(), MakeTraceId(), "INVALID_ARGUMENT",
                                      "request body is not valid JSON", false));
            return;
        }
        ProjectSnapshotState state;
        {
            std::lock_guard<std::mutex> lock(impl_->projectStateMutex);
            const auto it = impl_->projectStates.find(projectId);
            if (it == impl_->projectStates.end()) {
                res.status = 404;
                send(res, FailureEnvelope(MakeRequestId(), MakeTraceId(), "NOT_FOUND",
                                          "unknown project", false));
                return;
            }
            state = it->second;
        }
        // 显式 revision 必须与当前一致（多对象同版本约束）。
        if (body.contains("revision") && body["revision"].is_string() &&
            body["revision"].get<std::string>() != state.request.currentRevision) {
            res.status = 409;
            send(res, FailureEnvelope(MakeRequestId(), MakeTraceId(), "STALE_REVISION",
                                      "requested revision is not the current revision", false));
            return;
        }
        std::size_t maxBytes = 64 * 1024;
        if (body.contains("max_bytes") && body["max_bytes"].is_number_unsigned()) {
            maxBytes = body["max_bytes"].get<std::size_t>();
        }
        Json omitted = Json::array();
        Json data;
        data["project_id"] = projectId;
        data["revision"] = state.request.currentRevision;
        data["top"] = state.request.top;
        data["dirty"] = state.request.dirty;
        data["synchronized"] = state.request.synchronized;

        // needs：支持 sources（已登记源码片段）、selection（当前选择）、design（局部节点证据）、
        // buffer（未保存编辑缓冲，仅解释）。其余记入 omitted 并给出可读 reason。
        // 设计上下文与缓冲一律 usable_for_execution=false；缓冲永远不会成为执行输入。
        std::size_t used = 0;
        Json sources = Json::array();
        Json designNodes = Json::array();
        Json bufferExcerpts = Json::array();
        Json selectionData = nullptr;
        bool selectionRequested = false;
        bool designRequested = false;
        bool bufferRequested = false;
        std::vector<std::string> designNodeIds;
        if (body.contains("needs") && body["needs"].is_array()) {
            for (const auto& need : body["needs"]) {
                const std::string kind = need.is_string() ? need.get<std::string>()
                                                          : need.value("kind", std::string());
                if (kind == "selection") {
                    selectionRequested = true;
                    continue;
                }
                if (kind == "buffer") {
                    bufferRequested = true;
                    continue;
                }
                if (kind == "design") {
                    designRequested = true;
                    if (need.is_object() && need.contains("node_id") && need["node_id"].is_string()) {
                        designNodeIds.push_back(need["node_id"].get<std::string>());
                    }
                    continue;
                }
                if (kind != "sources") {
                    omitted.push_back(Json{{"kind", kind},
                                           {"reason", "unsupported need in this increment"}});
                    continue;
                }
                const std::string sourceId = need.is_object()
                                                 ? need.value("source_id", std::string())
                                                 : std::string();
                Json summary;
                for (const auto& entry : state.sourceSummary) {
                    if (entry.value("source_id", std::string()) == sourceId) {
                        summary = entry;
                        break;
                    }
                }
                if (summary.is_null()) {
                    omitted.push_back(Json{{"kind", kind},
                                           {"source_id", sourceId},
                                           {"reason", "unknown source id"}});
                    continue;
                }
                const std::string relativePath = summary.value("path", std::string());
                const std::filesystem::path absolute =
                    state.request.projectRoot / platform::PathFromUtf8(relativePath);
                const std::string currentHash = platform::Sha256FileHex(absolute);
                if (currentHash != summary.value("sha256", std::string())) {
                    omitted.push_back(Json{{"kind", kind},
                                           {"source_id", sourceId},
                                           {"reason", "source changed since context was built"}});
                    continue;
                }
                const std::uint64_t size = summary.value("size", std::string("0")) == "0"
                                               ? 0
                                               : std::stoull(summary.value("size", std::string("0")));
                if (used + size > maxBytes) {
                    omitted.push_back(Json{{"kind", kind},
                                           {"source_id", sourceId},
                                           {"reason", "exceeds max_bytes budget"}});
                    continue;
                }
                used += size;
                sources.push_back(Json{{"source_id", sourceId},
                                       {"path", relativePath},
                                       {"sha256", summary.value("sha256", std::string())},
                                       {"size", summary.value("size", std::string())},
                                       {"usable_for_execution", !state.request.dirty}});
            }
        }
        if (selectionRequested || designRequested || bufferRequested) {
            const DesignContextService::SelectionResolution selection =
                impl_->designContext.ResolveSelection(projectId, state.request.currentRevision,
                                                      state.request.projectRoot, bufferRequested);
            if (bufferRequested) {
                if (!selection.bufferAvailable) {
                    omitted.push_back(Json{{"kind", "buffer"},
                                           {"reason", selection.found ? "no_unsaved_buffer"
                                                                      : selection.reason}});
                } else {
                    for (const auto& excerpt : selection.selection.bufferExcerpts) {
                        const Json item = DesignContextService::BufferExcerptToJson(excerpt);
                        const std::size_t size = item.dump().size();
                        if (used + size > maxBytes) {
                            omitted.push_back(Json{{"kind", "buffer"},
                                                   {"reason", "exceeds max_bytes budget"}});
                            break;
                        }
                        used += size;
                        bufferExcerpts.push_back(item);
                    }
                }
            }
            if (selectionRequested) {
                if (!selection.found) {
                    omitted.push_back(Json{{"kind", "selection"}, {"reason", selection.reason}});
                } else {
                    Json refs = Json::array();
                    for (const auto& ref : selection.selection.sourceRefs) {
                        refs.push_back(Json{{"source_id", ref.sourceId},
                                            {"path", ref.path},
                                            {"file_hash", ref.fileHash},
                                            {"start_line", ref.startLine},
                                            {"end_line", ref.endLine}});
                    }
                    selectionData = Json{
                        {"revision", selection.selection.revision},
                        {"state_version", std::to_string(selection.selection.stateVersion)},
                        {"dirty", selection.selection.dirty},
                        {"buffer_hash", selection.selection.bufferHash.empty()
                                            ? Json(nullptr)
                                            : Json(selection.selection.bufferHash)},
                        {"selected_node_ids", selection.selection.selectedNodeIds},
                        {"source_refs", refs},
                        {"status", selection.reason},
                        {"usable_for_execution", false}};
                }
            }
            if (designRequested) {
                std::vector<DesignContextService::NodeResolution> resolved;
                if (!designNodeIds.empty()) {
                    for (const std::string& nodeId : designNodeIds) {
                        resolved.push_back(impl_->designContext.ResolveNode(
                            projectId, nodeId, state.request.currentRevision,
                            state.request.projectRoot));
                    }
                } else {
                    resolved = selection.nodes;
                }
                for (const auto& node : resolved) {
                    const Json item = DesignContextService::NodeToJson(
                        projectId, state.request.currentRevision, state.request.dirty, node);
                    const std::size_t size = item.dump().size();
                    if (used + size > maxBytes) {
                        omitted.push_back(Json{{"kind", "design"},
                                               {"node_id", node.node.nodeId},
                                               {"reason", "exceeds max_bytes budget"}});
                        continue;
                    }
                    used += size;
                    designNodes.push_back(item);
                }
                if (resolved.empty()) {
                    omitted.push_back(Json{{"kind", "design"},
                                           {"reason", selection.found ? "no_selection"
                                                                      : selection.reason}});
                }
            }
        }
        data["sources"] = sources;
        data["selection"] = selectionData;
        data["design_nodes"] = designNodes;
        data["buffer_excerpts"] = bufferExcerpts;
        data["omitted"] = omitted;
        data["used_bytes"] = used;
        data["max_bytes"] = maxBytes;
        res.status = 200;
        send(res, SuccessEnvelope(MakeRequestId(), MakeTraceId(), data));
    });

    // SF-08：GET /waves/{a}/signals —— 波形信号列表（分页 cursor，不暴露本机路径）。
    impl_->server->Get(R"(/api/v1/waves/([^/]+)/signals)",
                       [this, config, send](const httplib::Request& req, httplib::Response& res) {
        if (!PassesBoundary(req, config, res, send)) return;
        if (!IsAuthorized(req, config.token)) {
            res.status = 401;
            send(res, FailureEnvelope(MakeRequestId(), MakeTraceId(), "UNAUTHENTICATED",
                                      "missing or invalid bearer token", false));
            return;
        }
        const std::string artifactId = req.matches.size() > 1 ? req.matches[1].str() : "";
        std::size_t limit = 256;
        if (req.has_param("limit")) {
            try { limit = static_cast<std::size_t>(std::stoull(req.get_param_value("limit"))); }
            catch (...) { limit = 256; }
        }
        if (limit == 0 || limit > 4096) limit = 4096;
        const std::string cursor = req.has_param("cursor") ? req.get_param_value("cursor") : "";
        WaveSignalPage page;
        const WaveformService::Status status =
            impl_->waveformService.ListSignals(artifactId, cursor, limit, page);
        switch (status) {
            case WaveformService::Status::kNotFound:
                res.status = 404;
                send(res, FailureEnvelope(MakeRequestId(), MakeTraceId(), "NOT_FOUND",
                                          "unknown wave artifact", false));
                return;
            case WaveformService::Status::kExpired:
                res.status = 410;
                send(res, FailureEnvelope(MakeRequestId(), MakeTraceId(), "ARTIFACT_EXPIRED",
                                          "wave artifact is no longer available", false));
                return;
            case WaveformService::Status::kBadCursor:
                res.status = 400;
                send(res, FailureEnvelope(MakeRequestId(), MakeTraceId(), "INVALID_ARGUMENT",
                                          "invalid pagination cursor", false));
                return;
            case WaveformService::Status::kUnavailable:
                res.status = 503;
                send(res, FailureEnvelope(MakeRequestId(), MakeTraceId(), "PLUGIN_UNAVAILABLE",
                                          "waveform backend is not available", true));
                return;
            case WaveformService::Status::kOk:
                break;
        }
        Json signals = Json::array();
        for (const auto& signal : page.signals) {
            signals.push_back(Json{{"signal_id", signal.id},
                                   {"name", signal.name},
                                   {"scope", signal.scope},
                                   {"full_name", signal.fullName},
                                   {"width", signal.width},
                                   {"id_code", signal.idCode}});
        }
        Json data;
        data["artifact_id"] = artifactId;
        data["timescale"] = page.timescale;
        // 契约（schemas/wave-signals.schema.json）：time_range 为 {start_tick,end_tick}
        // 十进制字符串；时间范围不可信时为 null。64 位 tick 一律用十进制字符串承载。
        data["time_range"] =
            page.timeRange.valid
                ? Json{{"start_tick", std::to_string(page.timeRange.begin)},
                       {"end_tick", std::to_string(page.timeRange.end)}}
                : Json(nullptr);
        data["signals"] = signals;
        data["total"] = page.total;
        data["has_more"] = page.hasMore;
        data["next_cursor"] = page.hasMore ? Json(page.nextCursor) : Json(nullptr);
        res.status = 200;
        send(res, SuccessEnvelope(MakeRequestId(), MakeTraceId(), data));
    });

    // SF-08：POST /waves/{a}/query —— 精确区间跳变（初值 + transitions，显式限额/分页）。
    impl_->server->Post(R"(/api/v1/waves/([^/]+)/query)",
                        [this, config, send](const httplib::Request& req, httplib::Response& res) {
        if (!PassesBoundary(req, config, res, send)) return;
        if (!IsAuthorized(req, config.token)) {
            res.status = 401;
            send(res, FailureEnvelope(MakeRequestId(), MakeTraceId(), "UNAUTHENTICATED",
                                      "missing or invalid bearer token", false));
            return;
        }
        const std::string artifactId = req.matches.size() > 1 ? req.matches[1].str() : "";
        Json body;
        try {
            body = req.body.empty() ? Json::object() : Json::parse(req.body);
        } catch (const std::exception&) {
            res.status = 400;
            send(res, FailureEnvelope(MakeRequestId(), MakeTraceId(), "INVALID_ARGUMENT",
                                      "request body is not valid JSON", false));
            return;
        }
        if (!body.contains("start_tick") || !body["start_tick"].is_number_unsigned() ||
            !body.contains("end_tick") || !body["end_tick"].is_number_unsigned()) {
            res.status = 400;
            send(res, FailureEnvelope(MakeRequestId(), MakeTraceId(), "INVALID_ARGUMENT",
                                      "start_tick/end_tick must be unsigned integers", false));
            return;
        }
        WaveQueryRange range;
        range.startTick = body["start_tick"].get<std::uint64_t>();
        range.endTick = body["end_tick"].get<std::uint64_t>();
        if (range.endTick < range.startTick) {
            res.status = 400;
            send(res, FailureEnvelope(MakeRequestId(), MakeTraceId(), "INVALID_ARGUMENT",
                                      "end_tick precedes start_tick", false));
            return;
        }
        if (body.contains("signals") && body["signals"].is_array()) {
            for (const auto& s : body["signals"]) {
                if (s.is_number_integer()) range.signalIds.push_back(s.get<int>());
            }
        }
        const std::string cursor = body.contains("cursor") && body["cursor"].is_string()
                                       ? body["cursor"].get<std::string>()
                                       : std::string();
        WaveQueryPage page;
        // SF-08：载荷字节预算同时受普通响应限额约束（2 MiB），超限显式分页。
        const std::size_t queryBytes =
            std::min<std::size_t>(2 * 1024 * 1024, config.maxResponseBodyBytes);
        const WaveformService::Status status =
            impl_->waveformService.QueryRange(artifactId, range, cursor, 16, 10000, queryBytes,
                                              page);
        switch (status) {
            case WaveformService::Status::kNotFound:
                res.status = 404;
                send(res, FailureEnvelope(MakeRequestId(), MakeTraceId(), "NOT_FOUND",
                                          "unknown wave artifact", false));
                return;
            case WaveformService::Status::kExpired:
                res.status = 410;
                send(res, FailureEnvelope(MakeRequestId(), MakeTraceId(), "ARTIFACT_EXPIRED",
                                          "wave artifact is no longer available", false));
                return;
            case WaveformService::Status::kBadCursor:
                res.status = 400;
                send(res, FailureEnvelope(MakeRequestId(), MakeTraceId(), "INVALID_ARGUMENT",
                                          "invalid query cursor or range", false));
                return;
            case WaveformService::Status::kUnavailable:
                res.status = 503;
                send(res, FailureEnvelope(MakeRequestId(), MakeTraceId(), "PLUGIN_UNAVAILABLE",
                                          "waveform backend is not available", true));
                return;
            case WaveformService::Status::kOk:
                break;
        }
        Json signals = Json::array();
        for (const auto& s : page.signals) {
            Json transitions = Json::array();
            for (const auto& t : s.transitions) {
                transitions.push_back(Json{{"tick", std::to_string(t.time)}, {"value", t.value}});
            }
            signals.push_back(Json{{"signal_id", s.signalId},
                                   {"initial_value",
                                    s.hasInitial ? Json(s.initialValue) : Json(nullptr)},
                                   {"initial_known", s.hasInitial},
                                   {"transitions", transitions}});
        }
        const char* completeness = page.completeness == WaveQueryCompleteness::kExact
                                       ? "exact"
                                       : (page.completeness == WaveQueryCompleteness::kComplete
                                              ? "complete"
                                              : "partial");
        Json data;
        data["artifact_id"] = artifactId;
        data["start_tick"] = std::to_string(range.startTick);
        data["end_tick"] = std::to_string(range.endTick);
        data["timescale"] = page.timescale;
        data["signals"] = signals;
        data["completeness"] = completeness;
        data["has_more"] = page.hasMore;
        data["next_cursor"] = page.hasMore ? Json(page.nextCursor) : Json(nullptr);
        data["transition_count"] = page.transitionCount;
        data["omitted_signals"] = page.omittedSignals;
        res.status = 200;
        send(res, SuccessEnvelope(MakeRequestId(), MakeTraceId(), data));
    });

    // SF-06：GET /artifacts/{a} / GET /artifacts/{a}/content —— 受限元数据与内容读取。
    impl_->server->Get(R"(/api/v1/artifacts/([^/]+))",
                       [this, config, send](const httplib::Request& req, httplib::Response& res) {
        if (!PassesBoundary(req, config, res, send)) return;
        if (!IsAuthorized(req, config.token)) {
            res.status = 401;
            send(res, FailureEnvelope(MakeRequestId(), MakeTraceId(), "UNAUTHENTICATED",
                                      "missing or invalid bearer token", false));
            return;
        }
        const std::string artifactId = req.matches.size() > 1 ? req.matches[1].str() : "";
        ArtifactService::Metadata meta;
        const ArtifactService::Status status = impl_->artifactService.Describe(artifactId, meta);
        if (status == ArtifactService::Status::kNotFound) {
            res.status = 404;
            send(res, FailureEnvelope(MakeRequestId(), MakeTraceId(), "NOT_FOUND",
                                      "unknown artifact", false));
            return;
        }
        if (status == ArtifactService::Status::kExpired) {
            res.status = 410;
            send(res, FailureEnvelope(MakeRequestId(), MakeTraceId(), "ARTIFACT_EXPIRED",
                                      "artifact is no longer available", false));
            return;
        }
        Json data;
        data["artifact_id"] = meta.artifactId;
        data["project_id"] = meta.projectId;
        data["revision"] = meta.revision;
        data["job_id"] = meta.jobId;
        data["schema"] = meta.schema;
        data["sha256"] = meta.sha256;
        data["role"] = meta.role;
        data["size"] = std::to_string(meta.size);
        data["media_type"] = meta.mediaType;
        data["inline_readable"] = meta.inlineReadable;
        res.status = 200;
        send(res, SuccessEnvelope(MakeRequestId(), MakeTraceId(), data));
    });

    impl_->server->Get(R"(/api/v1/artifacts/([^/]+)/content)",
                       [this, config, send](const httplib::Request& req, httplib::Response& res) {
        if (!PassesBoundary(req, config, res, send)) return;
        if (!IsAuthorized(req, config.token)) {
            res.status = 401;
            send(res, FailureEnvelope(MakeRequestId(), MakeTraceId(), "UNAUTHENTICATED",
                                      "missing or invalid bearer token", false));
            return;
        }
        const std::string artifactId = req.matches.size() > 1 ? req.matches[1].str() : "";
        std::uint64_t offset = 0;
        std::uint64_t length = 0;
        if (req.has_param("offset")) {
            try { offset = std::stoull(req.get_param_value("offset")); } catch (...) { offset = 0; }
        }
        if (req.has_param("length")) {
            try { length = std::stoull(req.get_param_value("length")); } catch (...) { length = 0; }
        }
        std::string content;
        ArtifactService::Metadata meta;
        bool truncated = false;
        const ArtifactService::Status status = impl_->artifactService.ReadContent(
            artifactId, offset, length, ArtifactService::kDefaultContentLimit, content, meta,
            truncated);
        switch (status) {
            case ArtifactService::Status::kNotFound:
                res.status = 404;
                send(res, FailureEnvelope(MakeRequestId(), MakeTraceId(), "NOT_FOUND",
                                          "unknown artifact", false));
                return;
            case ArtifactService::Status::kExpired:
                res.status = 410;
                send(res, FailureEnvelope(MakeRequestId(), MakeTraceId(), "ARTIFACT_EXPIRED",
                                          "artifact is no longer available", false));
                return;
            case ArtifactService::Status::kUnsupported:
                res.status = 422;
                send(res, FailureEnvelope(MakeRequestId(), MakeTraceId(), "UNSUPPORTED_MAPPING",
                                          "artifact type is not inline-readable", false));
                return;
            case ArtifactService::Status::kBadRange:
                res.status = 400;
                send(res, FailureEnvelope(MakeRequestId(), MakeTraceId(), "INVALID_ARGUMENT",
                                          "offset/length out of range", false));
                return;
            case ArtifactService::Status::kOk:
                break;
        }
        Json data;
        data["artifact_id"] = meta.artifactId;
        data["schema"] = meta.schema;
        data["sha256"] = meta.sha256;
        data["media_type"] = meta.mediaType;
        data["offset"] = std::to_string(offset);
        data["size"] = std::to_string(meta.size);
        data["truncated"] = truncated;
        // 受限内容读取是 Agent 能看到工具原始输出的唯一通道；工具日志里常含本机路径，
        // 因此文本类内容同样裁剪并显式标记（不静默改写）。
        if (meta.mediaType.rfind("text/", 0) == 0 ||
            meta.mediaType == "application/json" || meta.mediaType.empty()) {
            const std::size_t redacted = RedactLocalPathsInText(content);
            if (redacted > 0) {
                data["path_redacted"] = true;
                data["redacted_paths"] = redacted;
            }
        }
        data["content"] = content;
        res.status = 200;
        send(res, SuccessEnvelope(MakeRequestId(), MakeTraceId(), data));
    });

    // SF-02：GET /projects/{p}/state —— 一致性快照（high_watermark 用于事件恢复）。
    impl_->server->Get(R"(/api/v1/projects/([^/]+)/state)",
                       [this, config, provider, send](const httplib::Request& req,
                                                      httplib::Response& res) {
        if (!PassesBoundary(req, config, res, send)) return;
        if (!IsAuthorized(req, config.token)) {
            res.status = 401;
            send(res, FailureEnvelope(MakeRequestId(), MakeTraceId(), "UNAUTHENTICATED",
                                      "missing or invalid bearer token", false));
            return;
        }
        const std::string projectId = req.matches.size() > 1 ? req.matches[1].str() : "";
        const std::vector<ReadyPlugin> ready = provider ? provider() : std::vector<ReadyPlugin>{};
        const Json capabilitiesData = BuildCapabilitiesData(
            config.instanceId, config.protocolVersion, config.edition,
            config.build.empty() ? BuildInfo() : config.build,
            ResolveEducationCapabilities(ready));
        Json data;
        data["project_id"] = projectId;
        data["high_watermark"] = std::to_string(impl_->events.HighWatermark());
        // SF-02：游标窗口下界。after < oldest_sequence 的订阅必然过期（410），
        // UI/Agent 先取 state + high_watermark 再续订（spec §5）。
        data["oldest_sequence"] = std::to_string(impl_->events.OldestSequence());
        data["event_retention"] = Json{{"max_events", 10000},
                                       {"max_age_seconds",
                                        std::to_string(EventStore::kDefaultMaxAgeSeconds)}};
        // 本项目仍在进行中的 Agent Job（不返回 params）。
        Json activeJobs = Json::array();
        {
            std::lock_guard<std::mutex> lock(impl_->jobMutex);
            for (const auto& binding : impl_->jobBindings) {
                const Json& b = binding.second;
                if (b.value("project_id", std::string()) != projectId) continue;
                Json summary;
                summary["job_id"] = binding.first;
                summary["job_type"] = b.value("job_type", std::string());
                summary["capability"] = b.value("capability", std::string());
                summary["state"] = b.value("state", std::string("unknown"));
                activeJobs.push_back(std::move(summary));
            }
        }
        data["active_jobs"] = std::move(activeJobs);
        data["capabilities"] = capabilitiesData.value("capabilities", Json::array());
        {
            std::lock_guard<std::mutex> lock(impl_->projectStateMutex);
            const auto it = impl_->projectStates.find(projectId);
            if (it != impl_->projectStates.end()) {
                data["context"] = Json{{"revision", it->second.request.currentRevision},
                                       {"dirty", it->second.request.dirty},
                                       {"synchronized", it->second.request.synchronized},
                                       {"top", it->second.request.top},
                                       {"target", it->second.request.target},
                                       {"sources", it->second.sourceSummary}};
            } else {
                data["context"] = nullptr;
            }
        }
        res.status = 200;
        send(res, SuccessEnvelope(MakeRequestId(), MakeTraceId(), data));
    });

    // SF-02：GET /projects/{p}/events?after=&wait_ms= —— 事件流（有界、游标恢复）。
    impl_->server->Get(R"(/api/v1/projects/([^/]+)/events)",
                       [this, config, send](const httplib::Request& req,
                                            httplib::Response& res) {
        if (!PassesBoundary(req, config, res, send)) return;
        if (!IsAuthorized(req, config.token)) {
            res.status = 401;
            send(res, FailureEnvelope(MakeRequestId(), MakeTraceId(), "UNAUTHENTICATED",
                                      "missing or invalid bearer token", false));
            return;
        }
        const std::string projectId = req.matches.size() > 1 ? req.matches[1].str() : "";
        const std::string afterText = req.has_param("after") ? req.get_param_value("after") : "0";
        std::uint64_t after = 0;
        try {
            after = std::stoull(afterText);
        } catch (const std::exception&) {
            res.status = 400;
            send(res, FailureEnvelope(MakeRequestId(), MakeTraceId(), "INVALID_ARGUMENT",
                                      "after must be a decimal cursor", false));
            return;
        }
        // SF-02：长轮询——若 after 之后暂无事件，最多等待 wait_ms（默认 20000，上限 25000）。
        int waitMs = 20000;
        if (req.has_param("wait_ms")) {
            try {
                waitMs = std::stoi(req.get_param_value("wait_ms"));
            } catch (const std::exception&) {
                waitMs = 20000;
            }
        }
        waitMs = std::max(0, std::min(waitMs, 25000));
        // SF-02：集合分页默认 100、最多 500；超限显式截断并给出 has_more。
        std::size_t limit = 100;
        if (req.has_param("limit")) {
            try {
                limit = static_cast<std::size_t>(std::stoull(req.get_param_value("limit")));
            } catch (const std::exception&) {
                limit = 100;
            }
        }
        if (limit == 0) limit = 100;
        if (limit > 500) limit = 500;
        impl_->events.WaitForEvents(projectId, after, waitMs);

        bool cursorExpired = false;
        const Json page = impl_->events.Read(projectId, after, limit + 1, cursorExpired);
        if (cursorExpired) {
            res.status = 410;
            send(res, FailureEnvelope(MakeRequestId(), MakeTraceId(), "CURSOR_EXPIRED",
                                      "cursor is older than the retained window", false));
            return;
        }
        Json data = page;
        const bool hasMore = data["events"].size() > limit;
        while (data["events"].size() > limit) data["events"].erase(data["events"].end() - 1);
        if (hasMore && !data["events"].empty()) {
            data["next_cursor"] = data["events"].back().value("sequence", data["next_cursor"]);
        }
        data["has_more"] = hasMore;
        res.status = 200;
        send(res, SuccessEnvelope(MakeRequestId(), MakeTraceId(), data));
    });

    // SF-02：POST /projects/{p}/grants —— 仅 UI 身份可签发（Agent token 禁止）。
    impl_->server->Post(R"(/api/v1/projects/([^/]+)/grants)",
                        [this, config, send](const httplib::Request& req, httplib::Response& res) {
        if (!PassesBoundary(req, config, res, send)) return;
        if (!IsUiAuthorized(req, config)) {
            res.status = 403;
            send(res, FailureEnvelope(MakeRequestId(), MakeTraceId(), "POLICY_DENIED",
                                      "grant issuance requires the UI identity", false));
            return;
        }
        const std::string projectId = req.matches.size() > 1 ? req.matches[1].str() : "";
        Json body;
        try {
            body = req.body.empty() ? Json::object() : Json::parse(req.body);
        } catch (const std::exception&) {
            res.status = 400;
            send(res, FailureEnvelope(MakeRequestId(), MakeTraceId(), "INVALID_ARGUMENT",
                                      "request body is not valid JSON", false));
            return;
        }
        const std::string planHash = body.value("plan_hash", std::string());
        if (planHash.empty()) {
            res.status = 400;
            send(res, FailureEnvelope(MakeRequestId(), MakeTraceId(), "INVALID_ARGUMENT",
                                      "plan_hash is required", false));
            return;
        }
        std::string planError;
        const Json planSteps = body.value("steps", Json::array());
        if (!ValidatePlanSteps(planSteps, planError)) {
            res.status = 400;
            send(res, FailureEnvelope(MakeRequestId(), MakeTraceId(), "INVALID_PLAN", planError,
                                      false));
            return;
        }
        Grant grant;
        std::string storeError;
        if (!impl_->grants.Issue(
                projectId, planHash, body.value("revision", std::string()),
                body.value("snapshot_id", std::string()),
                planSteps,
                body.value("ttl_seconds", static_cast<std::uint64_t>(600)),
                body.value("max_jobs", 3), grant, storeError)) {
            res.status = 503;
            send(res, FailureEnvelope(MakeRequestId(), MakeTraceId(), "SERVICE_UNAVAILABLE",
                                      "grant could not be persisted", true));
            return;
        }
        Json data;
        data["grant_id"] = grant.id;
        data["status"] = grant.status;
        data["plan_hash"] = grant.planHash;
        data["revision"] = grant.revision;
        data["snapshot_id"] = grant.snapshotId;
        data["expires_at"] = grant.expiresAt;
        data["max_jobs"] = grant.maxJobs;
        res.status = 201;
        send(res, SuccessEnvelope(MakeRequestId(), MakeTraceId(), data));
    });

    // SF-02：GET /grants/{id} —— Agent 可读自身 grant 状态。
    impl_->server->Get(R"(/api/v1/grants/([^/]+))",
                       [this, config, send](const httplib::Request& req, httplib::Response& res) {
        if (!PassesBoundary(req, config, res, send)) return;
        if (!IsAuthorized(req, config.token)) {
            res.status = 401;
            send(res, FailureEnvelope(MakeRequestId(), MakeTraceId(), "UNAUTHENTICATED",
                                      "missing or invalid bearer token", false));
            return;
        }
        const std::string grantId = req.matches.size() > 1 ? req.matches[1].str() : "";
        Grant grant;
        if (!impl_->grants.Lookup(grantId, grant)) {
            res.status = 404;
            send(res, FailureEnvelope(MakeRequestId(), MakeTraceId(), "NOT_FOUND",
                                      "unknown grant", false));
            return;
        }
        Json data;
        data["grant_id"] = grant.id;
        data["status"] = grant.status;
        data["project_id"] = grant.projectId;
        data["plan_hash"] = grant.planHash;
        data["revision"] = grant.revision;
        data["max_jobs"] = grant.maxJobs;
        data["expires_at"] = grant.expiresAt;
        res.status = 200;
        send(res, SuccessEnvelope(MakeRequestId(), MakeTraceId(), data));
    });

    // SF-02：POST /grants/{id}/revoke —— 仅 UI 身份。
    impl_->server->Post(R"(/api/v1/grants/([^/]+)/revoke)",
                        [this, config, send](const httplib::Request& req, httplib::Response& res) {
        if (!PassesBoundary(req, config, res, send)) return;
        if (!IsUiAuthorized(req, config)) {
            res.status = 403;
            send(res, FailureEnvelope(MakeRequestId(), MakeTraceId(), "POLICY_DENIED",
                                      "grant revocation requires the UI identity", false));
            return;
        }
        const std::string grantId = req.matches.size() > 1 ? req.matches[1].str() : "";
        std::string storeError;
        if (!impl_->grants.Revoke(grantId, storeError)) {
            res.status = storeError.empty() ? 404 : 503;
            send(res, FailureEnvelope(MakeRequestId(), MakeTraceId(),
                                      storeError.empty() ? "NOT_FOUND" : "SERVICE_UNAVAILABLE",
                                      storeError.empty() ? "unknown grant"
                                                         : "grant revocation could not be persisted",
                                      !storeError.empty()));
            return;
        }
        Json data;
        data["grant_id"] = grantId;
        data["status"] = "revoked";
        res.status = 200;
        send(res, SuccessEnvelope(MakeRequestId(), MakeTraceId(), data));
    });

    // SF-02：receipt —— UI 专用签发 / Agent 只读 / 原子一次性核销。
    impl_->server->Post(R"(/api/v1/projects/([^/]+)/ui-receipts)",
                        [this, config, send](const httplib::Request& req, httplib::Response& res) {
        if (!PassesBoundary(req, config, res, send)) return;
        if (!IsUiAuthorized(req, config)) {
            res.status = 403;
            send(res, FailureEnvelope(MakeRequestId(), MakeTraceId(), "POLICY_DENIED",
                                      "receipt issuance requires the UI identity", false));
            return;
        }
        const std::string projectId = req.matches.size() > 1 ? req.matches[1].str() : "";
        Json body;
        try {
            body = req.body.empty() ? Json::object() : Json::parse(req.body);
        } catch (const std::exception&) {
            res.status = 400;
            send(res, FailureEnvelope(MakeRequestId(), MakeTraceId(), "INVALID_ARGUMENT",
                                      "request body is not valid JSON", false));
            return;
        }
        const std::string sessionId = body.value("session_id", std::string());
        const std::string issue = body.value("issue", std::string());
        const std::string level = body.value("level", std::string());
        const std::string policy = body.value("policy", std::string());
        const std::string revision = body.value("revision", std::string());
        const std::string challengeId = body.value("challenge_id", std::string());
        const std::string actionId = body.value("action_id", std::string());
        std::uint64_t ttl = 900;
        if (body.contains("ttl_seconds") && body["ttl_seconds"].is_number_unsigned()) {
            ttl = body["ttl_seconds"].get<std::uint64_t>();
        }
        if (sessionId.empty() || issue.empty() || level.empty()) {
            res.status = 400;
            send(res, FailureEnvelope(MakeRequestId(), MakeTraceId(), "INVALID_ARGUMENT",
                                      "session_id/issue/level are required", false));
            return;
        }
        const bool levelSupported = level == "hint" || level == "l4" || level == "teaching";
        if (!levelSupported) {
            res.status = 400;
            send(res, FailureEnvelope(MakeRequestId(), MakeTraceId(), "INVALID_ARGUMENT",
                                      "level must be hint/l4/teaching", false));
            return;
        }
        UiReceipt receipt;
        std::string storeError;
        if (!impl_->receipts.Issue(projectId, sessionId, issue, level, policy, revision,
                                   challengeId, actionId, ttl, receipt, storeError)) {
            res.status = 503;
            send(res, FailureEnvelope(MakeRequestId(), MakeTraceId(), "SERVICE_UNAVAILABLE",
                                      storeError.empty() ? "receipt could not be issued"
                                                         : storeError,
                                      true));
            return;
        }
        Json data;
        data["receipt_id"] = receipt.id;
        data["project_id"] = receipt.projectId;
        data["session_id"] = receipt.sessionId;
        data["level"] = receipt.level;
        data["policy"] = receipt.policy;
        data["revision"] = receipt.revision;
        data["challenge_id"] = receipt.challengeId;
        data["action_id"] = receipt.actionId;
        data["status"] = receipt.status;
        data["state_version"] = std::to_string(receipt.stateVersion);
        data["expires_at"] = receipt.expiresAt;
        res.status = 201;
        send(res, SuccessEnvelope(MakeRequestId(), MakeTraceId(), data));
    });

    impl_->server->Get(R"(/api/v1/ui-receipts/([^/]+))",
                       [this, config, send](const httplib::Request& req, httplib::Response& res) {
        if (!PassesBoundary(req, config, res, send)) return;
        if (!IsAuthorized(req, config.token)) {
            res.status = 401;
            send(res, FailureEnvelope(MakeRequestId(), MakeTraceId(), "UNAUTHENTICATED",
                                      "missing or invalid bearer token", false));
            return;
        }
        const std::string receiptId = req.matches.size() > 1 ? req.matches[1].str() : "";
        UiReceipt receipt;
        if (!impl_->receipts.Lookup(receiptId, receipt)) {
            res.status = 404;
            send(res, FailureEnvelope(MakeRequestId(), MakeTraceId(), "NOT_FOUND",
                                      "unknown receipt", false));
            return;
        }
        Json data;
        data["receipt_id"] = receipt.id;
        data["project_id"] = receipt.projectId;
        data["session_id"] = receipt.sessionId;
        data["level"] = receipt.level;
        data["policy"] = receipt.policy;
        data["revision"] = receipt.revision;
        data["challenge_id"] = receipt.challengeId;
        data["action_id"] = receipt.actionId;
        data["status"] = receipt.status;
        data["expires_at"] = receipt.expiresAt;
        res.status = 200;
        send(res, SuccessEnvelope(MakeRequestId(), MakeTraceId(), data));
    });

    impl_->server->Post(R"(/api/v1/ui-receipts/([^/]+)/consume)",
                        [this, config, send](const httplib::Request& req, httplib::Response& res) {
        if (!PassesBoundary(req, config, res, send)) return;
        if (!IsAuthorized(req, config.token)) {
            res.status = 401;
            send(res, FailureEnvelope(MakeRequestId(), MakeTraceId(), "UNAUTHENTICATED",
                                      "missing or invalid bearer token", false));
            return;
        }
        const std::string receiptId = req.matches.size() > 1 ? req.matches[1].str() : "";
        Json body;
        try {
            body = req.body.empty() ? Json::object() : Json::parse(req.body);
        } catch (const std::exception&) {
            res.status = 400;
            send(res, FailureEnvelope(MakeRequestId(), MakeTraceId(), "INVALID_ARGUMENT",
                                      "request body is not valid JSON", false));
            return;
        }
        const std::string runId = body.value("run_id", std::string());
        const std::string actionId = body.value("action_id", std::string());
        if (runId.empty() || actionId.empty()) {
            res.status = 400;
            send(res, FailureEnvelope(MakeRequestId(), MakeTraceId(), "INVALID_ARGUMENT",
                                      "run_id/action_id are required", false));
            return;
        }
        std::uint64_t expectedStateVersion = 0;
        bool versionValid = false;
        if (body.contains("expected_state_version")) {
            if (body["expected_state_version"].is_number_unsigned()) {
                expectedStateVersion = body["expected_state_version"].get<std::uint64_t>();
                versionValid = true;
            } else if (body["expected_state_version"].is_string()) {
                try {
                    expectedStateVersion = std::stoull(body["expected_state_version"]
                                                           .get<std::string>());
                    versionValid = true;
                } catch (const std::exception&) {
                    versionValid = false;
                }
            }
        }
        if (!versionValid) {
            res.status = 400;
            send(res, FailureEnvelope(MakeRequestId(), MakeTraceId(), "INVALID_ARGUMENT",
                                      "expected_state_version must be a decimal string", false));
            return;
        }
        // 未知凭据在参数校验之后统一判定为 404（避免先报 400 掩盖伪造/过期）。
        {
            UiReceipt known;
            if (!impl_->receipts.Lookup(receiptId, known)) {
                res.status = 404;
                send(res, FailureEnvelope(MakeRequestId(), MakeTraceId(), "NOT_FOUND",
                                          "unknown receipt", false));
                return;
            }
        }
        const std::string projectId = body.value("project_id", std::string());
        ReceiptDecision decision;
        UiReceipt receipt;
        std::string storeError;
        const bool consumed =
            impl_->receipts.Consume(receiptId, projectId, runId, actionId, expectedStateVersion,
                                    decision, receipt, storeError);
        if (!consumed && decision != ReceiptDecision::Replayed) {
            const char* code = "POLICY_DENIED";
            int httpStatus = 403;
            if (decision == ReceiptDecision::NotFound) {
                code = "NOT_FOUND";
                httpStatus = 404;
            } else if (decision == ReceiptDecision::ActionMismatch ||
                       decision == ReceiptDecision::StateConflict) {
                code = "POLICY_DENIED";
                httpStatus = 409;
            } else if (decision == ReceiptDecision::StoreUnavailable) {
                code = "SERVICE_UNAVAILABLE";
                httpStatus = 503;
            }
            res.status = httpStatus;
            send(res, FailureEnvelope(MakeRequestId(), MakeTraceId(), code,
                                      storeError.empty() ? "receipt cannot be consumed"
                                                         : storeError,
                                      httpStatus == 503));
            return;
        }
        Json data;
        data["receipt_id"] = receipt.id;
        data["status"] = receipt.status;
        data["action_id"] = receipt.consumedActionId.empty() ? actionId : receipt.consumedActionId;
        data["consumed"] = true;
        data["replayed"] = decision == ReceiptDecision::Replayed;
        res.status = 200;
        send(res, SuccessEnvelope(MakeRequestId(), MakeTraceId(), data));
    });

    // SF-03：POST /projects/{p}/jobs —— 白名单能力 + grant 授权 + 幂等 + snapshot 归属。
    impl_->server->Post(R"(/api/v1/projects/([^/]+)/jobs)",
                        [this, config, send](const httplib::Request& req, httplib::Response& res) {
        if (!PassesBoundary(req, config, res, send)) return;
        if (!IsAuthorized(req, config.token)) {
            res.status = 401;
            send(res, FailureEnvelope(MakeRequestId(), MakeTraceId(), "UNAUTHENTICATED",
                                      "missing or invalid bearer token", false));
            return;
        }
        const std::string projectId = req.matches.size() > 1 ? req.matches[1].str() : "";
        // 幂等键必填（spec §5.2）。
        const std::string idempotencyKey = req.get_header_value("Idempotency-Key");
        if (idempotencyKey.empty()) {
            res.status = 400;
            send(res, FailureEnvelope(MakeRequestId(), MakeTraceId(), "INVALID_ARGUMENT",
                                      "Idempotency-Key header is required", false));
            return;
        }
        Json body;
        try {
            body = req.body.empty() ? Json::object() : Json::parse(req.body);
        } catch (const std::exception&) {
            res.status = 400;
            send(res, FailureEnvelope(MakeRequestId(), MakeTraceId(), "INVALID_ARGUMENT",
                                      "request body is not valid JSON", false));
            return;
        }
        if (!body.is_object()) {
            res.status = 400;
            send(res, FailureEnvelope(MakeRequestId(), MakeTraceId(), "INVALID_ARGUMENT",
                                      "request body must be a JSON object", false));
            return;
        }
        static const std::set<std::string> kJobRequestFields{
            "snapshot_id", "expected_revision", "capability", "grant_id", "plugin_id", "params",
            "plan_hash", "step_id"};
        for (auto field = body.begin(); field != body.end(); ++field) {
            if (kJobRequestFields.find(field.key()) == kJobRequestFields.end()) {
                res.status = 400;
                send(res, FailureEnvelope(MakeRequestId(), MakeTraceId(), "INVALID_ARGUMENT",
                                          "unknown job request field", false));
                return;
            }
        }

        // 能力白名单 + jobType 映射。
        const std::string capability = OptionalString(body, "capability");
        std::string jobType;
        for (const auto& mapping : EducationCapabilities()) {
            if (mapping.capability == capability) {
                jobType = mapping.jobType;
                break;
            }
        }
        if (jobType.empty()) {
            res.status = 422;
            send(res, FailureEnvelope(MakeRequestId(), MakeTraceId(), "CAPABILITY_UNAVAILABLE",
                                      "capability is not available to the educational agent", false));
            return;
        }
        const std::vector<ReadyPlugin> ready =
            impl_->provider ? impl_->provider() : std::vector<ReadyPlugin>{};
        const std::vector<CapabilityStatus> availability = ResolveEducationCapabilities(ready);
        const auto available = std::find_if(
            availability.begin(), availability.end(), [&capability](const CapabilityStatus& item) {
                return item.id == capability && item.ready && !item.disabled;
            });
        if (available == availability.end()) {
            res.status = 422;
            send(res, FailureEnvelope(MakeRequestId(), MakeTraceId(), "CAPABILITY_UNAVAILABLE",
                                      "no ready plugin provides the requested capability", false));
            return;
        }

        const std::string grantId = OptionalString(body, "grant_id");
        const std::string snapshotId = OptionalString(body, "snapshot_id");
        const std::string expectedRevision = OptionalString(body, "expected_revision");
        const auto paramsIt = body.find("params");
        const Json params = paramsIt == body.end() ? Json::object() : *paramsIt;
        // 请求形状/必填授权标识不具有任何副作用，必须在幂等登记前拒绝，
        // 避免无效请求把同一 Idempotency-Key 留在 InFlight。
        if (grantId.empty()) {
            res.status = 403;
            send(res, FailureEnvelope(MakeRequestId(), MakeTraceId(), "POLICY_DENIED",
                                      "a valid grant is required to submit jobs", false));
            return;
        }
        if (snapshotId.empty() || expectedRevision.empty() || !params.is_object()) {
            res.status = 400;
            send(res, FailureEnvelope(MakeRequestId(), MakeTraceId(), "INVALID_ARGUMENT",
                                      "snapshot_id, expected_revision and object params are required",
                                      false));
            return;
        }
        // SF-03：能力参数白名单（无副作用，幂等登记前拒绝）。
        std::string paramError;
        if (!ValidateCapabilityParams(capability, params, paramError)) {
            res.status = 422;
            send(res, FailureEnvelope(MakeRequestId(), MakeTraceId(), "UNSUPPORTED_MAPPING",
                                      paramError, false));
            return;
        }

        // Job 必须锚定当前可执行工程状态以及已提交、同 revision 的快照。
        // 这些只读检查在幂等登记之前执行，失败不会留下 InFlight 键。
        ProjectSnapshotState projectState;
        {
            std::lock_guard<std::mutex> lock(impl_->projectStateMutex);
            const auto state = impl_->projectStates.find(projectId);
            if (state == impl_->projectStates.end()) {
                res.status = 404;
                send(res, FailureEnvelope(MakeRequestId(), MakeTraceId(), "NOT_FOUND",
                                          "unknown project", false));
                return;
            }
            projectState = state->second;
        }
        if (projectState.request.dirty || !projectState.request.synchronized ||
            projectState.request.currentRevision != expectedRevision) {
            res.status = 409;
            send(res, FailureEnvelope(MakeRequestId(), MakeTraceId(), "STALE_REVISION",
                                      "project is not saved, synchronized and at the expected revision",
                                      false));
            return;
        }
        SnapshotRecord snapshot;
        std::string snapshotError;
        SnapshotService snapshotService(projectState.request.projectRoot /
                                        ".sigflow" / "agent" / "snapshots");
        if (!snapshotService.Lookup(snapshotId, snapshot, snapshotError)) {
            // 区分"从未存在"与"曾经存在但已被清理/失效"：
            // id 合法但目录已不在（保留策略删除）→ 410 ARTIFACT_EXPIRED，
            // 这样调用方能判断"旧引用已过期"，而不是误以为 id 打错。
            const bool wellFormedId = snapshotId.rfind("snap-", 0) == 0 && snapshotId.size() > 5;
            std::error_code goneError;
            const bool directoryGone =
                !std::filesystem::exists(snapshotService.SnapshotDirectory(snapshotId), goneError);
            if (wellFormedId && directoryGone) {
                res.status = 410;
                send(res, FailureEnvelope(MakeRequestId(), MakeTraceId(), "ARTIFACT_EXPIRED",
                                          "snapshot was pruned or is no longer available", false));
                return;
            }
            res.status = 404;
            send(res, FailureEnvelope(MakeRequestId(), MakeTraceId(), "NOT_FOUND",
                                      "snapshot is unavailable", false));
            return;
        }
        if (snapshot.projectId != projectId || snapshot.revision != expectedRevision) {
            res.status = 409;
            send(res, FailureEnvelope(MakeRequestId(), MakeTraceId(), "STALE_REVISION",
                                      "snapshot does not belong to the expected project revision", false));
            return;
        }

        // 幂等登记（scope=project+capability；hash 含 snapshot/capability/params）。
        // 必须先于 grant 配额消耗：同键重放返回同一 job，不得再次扣减 max_jobs。
        const Json canonical{{"snapshot_id", snapshotId},
                              {"capability", capability},
                              {"plugin_id", OptionalString(body, "plugin_id")},
                              {"params", params}};
        const std::string requestHash = IdempotencyStore::HashRequest(canonical);
        const std::string idempotencyScope = projectId + "|" + capability;
        IdempotencyStore::Entry entry;
        const IdempotencyStore::Outcome outcome =
            impl_->idempotency.Begin(idempotencyScope, idempotencyKey, requestHash, entry);
        if (outcome == IdempotencyStore::Outcome::Conflict) {
            res.status = 409;
            send(res, FailureEnvelope(MakeRequestId(), MakeTraceId(), "IDEMPOTENCY_CONFLICT",
                                      "same Idempotency-Key with different payload", false));
            return;
        }
        if (outcome == IdempotencyStore::Outcome::Replay) {
            Json data;
            data["job_id"] = entry.resourceId;
            data["state"] = "Queued";
            data["replayed"] = true;
            res.status = 202;
            send(res, SuccessEnvelope(MakeRequestId(), MakeTraceId(), data));
            return;
        }
        if (outcome == IdempotencyStore::Outcome::InFlight) {
            res.status = 409;
            send(res, FailureEnvelope(MakeRequestId(), MakeTraceId(), "RECOVERY_REQUIRED",
                                      "request is already in flight; query before retrying", false));
            return;
        }

        // 服务存在性不产生副作用；先检查，避免服务未接入时消耗 grant 配额。
        IJobService* service = impl_->jobServiceProvider ? impl_->jobServiceProvider() : nullptr;
        if (service == nullptr) {
            impl_->idempotency.Abandon(idempotencyScope, idempotencyKey, requestHash);
            res.status = 503;
            send(res, FailureEnvelope(MakeRequestId(), MakeTraceId(), "SERVICE_UNAVAILABLE",
                                      "job service is not available", true));
            return;
        }

        // SF-03：sim.run 只接受本快照上成功 sim.build 登记的执行产物；不接受绝对路径。
        if (capability == "eda.sim.run") {
            const std::string buildJobId = OptionalString(params, "build_job_id");
            Json buildBinding;
            {
                std::lock_guard<std::mutex> lock(impl_->jobMutex);
                const auto it = impl_->jobBindings.find(buildJobId);
                if (it != impl_->jobBindings.end()) buildBinding = it->second;
            }
            bool valid = !buildJobId.empty() && !buildBinding.is_null();
            if (valid && buildBinding.value("project_id", std::string()) != projectId) {
                valid = false;
            }
            if (valid && buildBinding.value("snapshot_id", std::string()) != snapshotId) {
                valid = false;
            }
            if (valid &&
                buildBinding.value("capability", std::string()) != "eda.sim.build") {
                valid = false;
            }
            if (valid) {
                const auto buildRecord = service->get(buildJobId);
                valid = buildRecord.has_value() && buildRecord->state == JobState::Succeeded;
            }
            if (!valid) {
                impl_->idempotency.Abandon(idempotencyScope, idempotencyKey, requestHash);
                res.status = buildJobId.empty() ? 400 : 422;
                send(res, FailureEnvelope(
                              MakeRequestId(), MakeTraceId(),
                              buildJobId.empty() ? "INVALID_ARGUMENT" : "UNSUPPORTED_MAPPING",
                              buildJobId.empty()
                                  ? "eda.sim.run requires build_job_id of a prior sim.build"
                                  : "build_job_id must reference a succeeded sim.build on the same snapshot",
                              false));
                return;
            }
        }

        // Read the grant before consuming quota so a stepped plan can authorize
        // this exact capability/parameter/dependency edge rather than merely
        // any of N jobs under the same snapshot.
        Grant checkedGrant;
        const GrantDecision checkedDecision = impl_->grants.Check(grantId, projectId, &checkedGrant);
        if (checkedDecision != GrantDecision::Ok) {
            impl_->idempotency.Abandon(idempotencyScope, idempotencyKey, requestHash);
            res.status = checkedDecision == GrantDecision::NotFound ? 404 : 403;
            send(res, FailureEnvelope(MakeRequestId(), MakeTraceId(),
                                      checkedDecision == GrantDecision::NotFound ? "NOT_FOUND"
                                                                                : "POLICY_DENIED",
                                      "grant is unavailable for plan validation", false));
            return;
        }
        std::unordered_map<std::string, Json> bindingSnapshot;
        {
            std::lock_guard<std::mutex> lock(impl_->jobMutex);
            bindingSnapshot = impl_->jobBindings;
        }
        std::string planError;
        if (!ValidateGrantStep(checkedGrant, body, capability, params, service, projectId,
                               snapshotId, bindingSnapshot, planError)) {
            impl_->idempotency.Abandon(idempotencyScope, idempotencyKey, requestHash);
            res.status = 403;
            send(res, FailureEnvelope(MakeRequestId(), MakeTraceId(), "POLICY_DENIED",
                                      planError, false));
            return;
        }

        // Build all provider-facing input before consuming a grant.  This work
        // is read-only, so invalid mappings cannot burn a student's quota.
        Json trustedParams;
        std::string trustedParamsError;
        if (!BuildTrustedJobParams(capability, params, snapshot, snapshotService, service,
                                   trustedParams, trustedParamsError)) {
            impl_->idempotency.Abandon(idempotencyScope, idempotencyKey, requestHash);
            res.status = 422;
            send(res, FailureEnvelope(MakeRequestId(), MakeTraceId(), "UNSUPPORTED_MAPPING",
                                      trustedParamsError, false));
            return;
        }

        // grant 授权 + 配额消耗（原子；仅在 Fresh 幂等路径上执行）。
        GrantDecision grantDecision = GrantDecision::NotFound;
        std::string grantError;
        if (!impl_->grants.Consume(grantId, projectId, grantDecision, grantError,
                                   expectedRevision, snapshotId)) {
            impl_->idempotency.Abandon(idempotencyScope, idempotencyKey, requestHash);
            const char* code = "POLICY_DENIED";
            if (grantDecision == GrantDecision::NotFound) code = "NOT_FOUND";
            else if (grantDecision == GrantDecision::Expired) code = "POLICY_DENIED";
            else if (grantDecision == GrantDecision::ProjectExhausted) code = "RESOURCE_EXHAUSTED";
            res.status = grantDecision == GrantDecision::NotFound ? 404 : 403;
            send(res, FailureEnvelope(MakeRequestId(), MakeTraceId(), code, grantError, false));
            return;
        }

        JobRequest jobRequest;
        jobRequest.jobType = jobType;
        jobRequest.projectId = projectId;
        // 只把 Gateway 推导的可信参数交给 provider；HTTP 请求中的 params 不含路径，
        // 也不会被原样持久化进 Job manifest。
        jobRequest.params = std::move(trustedParams);
        // Persist only Gateway-derived provenance, never the raw HTTP body or
        // bearer credentials.  CoreJobService writes this before exposing the
        // job id, so Gateway can reconstruct ownership after an IDE restart.
        jobRequest.metadata = Json{{"schema", "sigflow.edu-agent-job-binding.v1"},
                                   {"instance_id", config.instanceId},
                                   {"project_id", projectId},
                                   {"snapshot_id", snapshotId},
                                   {"revision", expectedRevision},
                                   {"capability", capability},
                                   {"plan_hash", checkedGrant.planHash},
                                   {"step_id", OptionalString(body, "step_id")},
                                   {"idempotency_scope", idempotencyScope},
                                   {"idempotency_key", idempotencyKey},
                                   {"request_hash", requestHash}};
        jobRequest.requireConfirm = false;
        const std::string jobId = service->submit(jobRequest);
        if (jobId.empty()) {
            // Grant consumption has committed.  A nonconforming service could
            // theoretically have accepted work but lost its response, so leave
            // the idempotency key InFlight and require recovery rather than
            // abandoning it and permitting an unsafe automatic retry.
            res.status = 503;
            send(res, FailureEnvelope(MakeRequestId(), MakeTraceId(), "RECOVERY_REQUIRED",
                                      "job submission outcome is unknown; query before retrying", true));
            return;
        }
        impl_->idempotency.Complete(idempotencyScope, idempotencyKey, jobId);
        {
            std::lock_guard<std::mutex> lock(impl_->jobMutex);
            impl_->jobProjects[jobId] = projectId;
            impl_->jobBindings[jobId] = Json{{"project_id", projectId},
                                             {"snapshot_id", snapshotId},
                                             {"revision", expectedRevision},
                                             {"capability", capability},
                                             {"job_type", jobType},
                                             {"state", "Queued"}};
        }
        // SF-02：登记到事件源，后续 PollJobEvents 才能观察到该 Job 的状态变化。
        impl_->jobEvents.Track(jobId, projectId, capability, snapshotId, expectedRevision);
        const Json event = impl_->events.Append(
            "job/created", projectId, MakeTraceId(),
            Json{{"job_id", jobId}, {"capability", capability}, {"snapshot_id", snapshotId}});
        if (!config.eventLogPath.empty()) impl_->events.AppendToFile(config.eventLogPath, event);

        Json data;
        data["job_id"] = jobId;
        data["state"] = "Queued";
        data["revision"] = expectedRevision;
        res.status = 202;
        send(res, SuccessEnvelope(MakeRequestId(), MakeTraceId(), data));
    });

    // SF-03：GET /jobs/{j} —— 按 ID 查询（归属以 project 校验）。
    impl_->server->Get(R"(/api/v1/jobs/([^/]+))",
                       [this, config, send](const httplib::Request& req, httplib::Response& res) {
        if (!PassesBoundary(req, config, res, send)) return;
        if (!IsAuthorized(req, config.token)) {
            res.status = 401;
            send(res, FailureEnvelope(MakeRequestId(), MakeTraceId(), "UNAUTHENTICATED",
                                      "missing or invalid bearer token", false));
            return;
        }
        const std::string jobId = req.matches.size() > 1 ? req.matches[1].str() : "";
        std::string owner;
        {
            std::lock_guard<std::mutex> lock(impl_->jobMutex);
            const auto it = impl_->jobProjects.find(jobId);
            if (it != impl_->jobProjects.end()) owner = it->second;
        }
        if (owner.empty()) {
            res.status = 404;
            send(res, FailureEnvelope(MakeRequestId(), MakeTraceId(), "NOT_FOUND",
                                      "unknown agent job", false));
            return;
        }
        IJobService* service = impl_->jobServiceProvider ? impl_->jobServiceProvider() : nullptr;
        if (service == nullptr) {
            res.status = 503;
            send(res, FailureEnvelope(MakeRequestId(), MakeTraceId(), "SERVICE_UNAVAILABLE",
                                      "job service is not available", true));
            return;
        }
        const std::optional<JobRecord> record = service->get(jobId);
        if (!record.has_value()) {
            res.status = 404;
            send(res, FailureEnvelope(MakeRequestId(), MakeTraceId(), "NOT_FOUND",
                                      "unknown job", false));
            return;
        }
        const auto stateName = [](JobState state) -> const char* {
            switch (state) {
                case JobState::Created: return "Created";
                case JobState::Validating: return "Validating";
                case JobState::Queued: return "Queued";
                case JobState::Running: return "Running";
                case JobState::ValidatingArtifact: return "ValidatingArtifact";
                case JobState::Succeeded: return "Succeeded";
                case JobState::Failed: return "Failed";
                case JobState::Cancelled: return "Cancelled";
                case JobState::TimedOut: return "TimedOut";
            }
            return "Unknown";
        };
        Json data;
        data["job_id"] = jobId;
        data["project_id"] = owner;
        data["job_type"] = record->request.jobType;
        data["state"] = stateName(record->state);
        data["exit_code"] = record->exitCode;
        data["created_at"] = record->createdAt;
        data["updated_at"] = record->updatedAt;
        {
            // SF-02：保持绑定中的状态最新（/state 的 active_jobs 据此反映进行中 Job）。
            std::lock_guard<std::mutex> lock(impl_->jobMutex);
            const auto it = impl_->jobBindings.find(jobId);
            if (it != impl_->jobBindings.end()) it->second["state"] = data["state"];
        }
        res.status = 200;
        send(res, SuccessEnvelope(MakeRequestId(), MakeTraceId(), data));
    });

    // SF-03：GET /jobs/{j}/report —— 归一化报告视图（SF-06 归一化在后续增量）。
    impl_->server->Get(R"(/api/v1/jobs/([^/]+)/report)",
                       [this, config, send](const httplib::Request& req, httplib::Response& res) {
        if (!PassesBoundary(req, config, res, send)) return;
        if (!IsAuthorized(req, config.token)) {
            res.status = 401;
            send(res, FailureEnvelope(MakeRequestId(), MakeTraceId(), "UNAUTHENTICATED",
                                      "missing or invalid bearer token", false));
            return;
        }
        const std::string jobId = req.matches.size() > 1 ? req.matches[1].str() : "";
        {
            std::lock_guard<std::mutex> lock(impl_->jobMutex);
            if (impl_->jobProjects.find(jobId) == impl_->jobProjects.end()) {
                res.status = 404;
                send(res, FailureEnvelope(MakeRequestId(), MakeTraceId(), "NOT_FOUND",
                                          "unknown agent job", false));
                return;
            }
        }
        IJobService* service = impl_->jobServiceProvider ? impl_->jobServiceProvider() : nullptr;
        if (service == nullptr) {
            res.status = 503;
            send(res, FailureEnvelope(MakeRequestId(), MakeTraceId(), "SERVICE_UNAVAILABLE",
                                      "job service is not available", true));
            return;
        }
        const JobReport rawReport = service->report(jobId);
        if (rawReport.jobId.empty()) {
            res.status = 404;
            send(res, FailureEnvelope(MakeRequestId(), MakeTraceId(), "NOT_FOUND",
                                      "unknown job report", false));
            return;
        }
        Json binding;
        {
            std::lock_guard<std::mutex> lock(impl_->jobMutex);
            const auto bindingIt = impl_->jobBindings.find(jobId);
            if (bindingIt == impl_->jobBindings.end()) {
                res.status = 404;
                send(res, FailureEnvelope(MakeRequestId(), MakeTraceId(), "NOT_FOUND",
                                          "job binding is unavailable", false));
                return;
            }
            binding = bindingIt->second;
        }

        // Convert plugin-local artifact names into opaque Gateway ids and register
        // their metadata before serializing the report.  This is the only route
        // by which an Agent can subsequently ask for bounded artifact/wave data.
        JobReport report = rawReport;
        const std::string projectId = binding.value("project_id", std::string());
        const std::string revision = binding.value("revision", std::string());
        for (Artifact& artifact : report.artifacts) {
            const std::string originalId = artifact.id;
            artifact.id = GatewayArtifactId(jobId, artifact);
            ArtifactRecord record;
            record.artifactId = artifact.id;
            record.projectId = projectId;
            record.revision = revision;
            record.jobId = jobId;
            record.path = artifact.path;
            record.schema = artifact.schema;
            record.sha256 = artifact.sha256;
            record.role = originalId.empty() ? artifact.role : originalId;
            impl_->artifactService.Register(record);
            if (IsWaveArtifact(artifact)) {
                WaveArtifactRecord wave;
                wave.artifactId = artifact.id;
                wave.projectId = projectId;
                wave.revision = revision;
                wave.jobId = jobId;
                wave.path = artifact.path;
                wave.sha256 = artifact.sha256;
                wave.schema = artifact.schema;
                impl_->waveformService.Register(wave);
            }
        }

        SnapshotRecord snapshot;
        bool verifiedSnapshot = false;
        const std::string snapshotId = binding.value("snapshot_id", std::string());
        std::filesystem::path projectRoot;
        {
            std::lock_guard<std::mutex> lock(impl_->projectStateMutex);
            const auto stateIt = impl_->projectStates.find(projectId);
            if (stateIt != impl_->projectStates.end()) projectRoot = stateIt->second.request.projectRoot;
        }
        std::string snapshotError;
        if (!projectRoot.empty() && !snapshotId.empty()) {
            SnapshotService snapshots(projectRoot / ".sigflow" / "agent" / "snapshots");
            verifiedSnapshot = snapshots.Lookup(snapshotId, snapshot, snapshotError) &&
                               snapshot.projectId == projectId && snapshot.revision == revision;
        }

        std::string pluginVersion;
        const std::string capability = binding.value("capability", std::string());
        for (const CapabilityStatus& status : ResolveEducationCapabilities(
                 impl_->provider ? impl_->provider() : std::vector<ReadyPlugin>{})) {
            if (status.id == capability && status.pluginId == rawReport.pluginId) {
                pluginVersion = status.pluginVersion;
                break;
            }
        }

        // SF-06：归一化为 edu.jobreport.v1，并填入可信 job binding / snapshot 事实。
        ReportNormalizeInput normalizeInput;
        normalizeInput.report = report;
        normalizeInput.projectId = projectId;
        normalizeInput.revision = revision;
        normalizeInput.snapshotId = snapshotId;
        normalizeInput.capability = capability;
        normalizeInput.pluginVersion = pluginVersion;
        normalizeInput.inputFingerprint = verifiedSnapshot ? snapshot.inputFingerprint : std::string();
        normalizeInput.legacyUnverified = !verifiedSnapshot;
        if (!verifiedSnapshot) {
            normalizeInput.rawReportSchema = "gateway-agent-binding-unverified";
        }
        const Json data = NormalizeJobReport(normalizeInput);
        res.status = 200;
        send(res, SuccessEnvelope(MakeRequestId(), MakeTraceId(), data));
    });

    // SF-03：POST /jobs/{j}/cancel —— 只允许本会话授权范围任务。
    impl_->server->Post(R"(/api/v1/jobs/([^/]+)/cancel)",
                        [this, config, send](const httplib::Request& req, httplib::Response& res) {
        if (!PassesBoundary(req, config, res, send)) return;
        if (!IsAuthorized(req, config.token)) {
            res.status = 401;
            send(res, FailureEnvelope(MakeRequestId(), MakeTraceId(), "UNAUTHENTICATED",
                                      "missing or invalid bearer token", false));
            return;
        }
        const std::string jobId = req.matches.size() > 1 ? req.matches[1].str() : "";
        {
            std::lock_guard<std::mutex> lock(impl_->jobMutex);
            if (impl_->jobProjects.find(jobId) == impl_->jobProjects.end()) {
                res.status = 404;
                send(res, FailureEnvelope(MakeRequestId(), MakeTraceId(), "NOT_FOUND",
                                          "unknown agent job", false));
                return;
            }
        }
        IJobService* service = impl_->jobServiceProvider ? impl_->jobServiceProvider() : nullptr;
        if (service == nullptr) {
            res.status = 503;
            send(res, FailureEnvelope(MakeRequestId(), MakeTraceId(), "SERVICE_UNAVAILABLE",
                                      "job service is not available", true));
            return;
        }
        const bool accepted = service->cancel(jobId, "agent cancel");
        if (!accepted) {
            res.status = 409;
            send(res, FailureEnvelope(MakeRequestId(), MakeTraceId(), "POLICY_DENIED",
                                      "job cannot be cancelled (unknown or terminal)", false));
            return;
        }
        Json data;
        data["job_id"] = jobId;
        data["accepted"] = true;  // ack 不代表进程已退出（spec §5.2）
        res.status = 200;
        send(res, SuccessEnvelope(MakeRequestId(), MakeTraceId(), data));
    });

    // SF-03：GET /projects/{p}/jobs —— 按项目列出 Agent Job（集合分页）。
    impl_->server->Get(R"(/api/v1/projects/([^/]+)/jobs)",
                       [this, config, send](const httplib::Request& req, httplib::Response& res) {
        if (!PassesBoundary(req, config, res, send)) return;
        if (!IsAuthorized(req, config.token)) {
            res.status = 401;
            send(res, FailureEnvelope(MakeRequestId(), MakeTraceId(), "UNAUTHENTICATED",
                                      "missing or invalid bearer token", false));
            return;
        }
        const std::string projectId = req.matches.size() > 1 ? req.matches[1].str() : "";
        {
            // 项目是否存在（不可变 DTO 判定）。
            std::lock_guard<std::mutex> lock(impl_->projectStateMutex);
            if (impl_->projectStates.find(projectId) == impl_->projectStates.end()) {
                res.status = 404;
                send(res, FailureEnvelope(MakeRequestId(), MakeTraceId(), "NOT_FOUND",
                                          "unknown project", false));
                return;
            }
        }
        IJobService* service = impl_->jobServiceProvider ? impl_->jobServiceProvider() : nullptr;
        if (service == nullptr) {
            res.status = 503;
            send(res, FailureEnvelope(MakeRequestId(), MakeTraceId(), "SERVICE_UNAVAILABLE",
                                      "job service is not available", true));
            return;
        }
        // 收集本项目 Agent 提交的 job id（按提交顺序）。
        std::vector<std::string> ids;
        {
            std::lock_guard<std::mutex> lock(impl_->jobMutex);
            for (const auto& binding : impl_->jobBindings) {
                if (binding.second.value("project_id", std::string()) == projectId) {
                    ids.push_back(binding.first);
                }
            }
        }
        std::sort(ids.begin(), ids.end());
        std::size_t limit = 100;
        if (req.has_param("limit")) {
            try {
                limit = static_cast<std::size_t>(std::stoull(req.get_param_value("limit")));
            } catch (const std::exception&) {
                limit = 100;
            }
        }
        if (limit == 0) limit = 100;
        if (limit > 500) limit = 500;
        // 简单 offset 分页（job 列表量级有限，offset 足够）。
        std::size_t offset = 0;
        if (req.has_param("offset")) {
            try {
                offset = static_cast<std::size_t>(std::stoull(req.get_param_value("offset")));
            } catch (const std::exception&) {
                offset = 0;
            }
        }
        Json jobs = Json::array();
        const std::size_t begin = std::min(offset, ids.size());
        const std::size_t end = std::min(ids.size(), begin + limit);
        for (std::size_t i = begin; i < end; ++i) {
            const std::string& id = ids[i];
            const auto record = service->get(id);
            if (!record.has_value()) continue;
            std::string bindingCapability;
            {
                std::lock_guard<std::mutex> lock(impl_->jobMutex);
                const auto it = impl_->jobBindings.find(id);
                if (it != impl_->jobBindings.end()) {
                    bindingCapability = it->second.value("capability", std::string());
                }
            }
            Json summary;
            summary["job_id"] = id;
            summary["job_type"] = record->request.jobType;
            summary["plugin_id"] = record->request.pluginId;
            summary["capability"] = bindingCapability;
            summary["state"] = JobStateName(record->state);
            summary["exit_code"] = record->exitCode;
            summary["created_at"] = record->createdAt;
            summary["updated_at"] = record->updatedAt;
            jobs.push_back(summary);
            // SF-02：把最新状态回填到绑定，供 GET /projects/{p}/state 的 active_jobs 使用。
            {
                std::lock_guard<std::mutex> lock(impl_->jobMutex);
                const auto it = impl_->jobBindings.find(id);
                if (it != impl_->jobBindings.end()) {
                    it->second["state"] = summary["state"];
                }
            }
        }
        Json data;
        data["jobs"] = jobs;
        data["total"] = ids.size();
        data["offset"] = offset;
        data["has_more"] = end < ids.size();
        res.status = 200;
        send(res, SuccessEnvelope(MakeRequestId(), MakeTraceId(), data));
    });

    // NG-09：旧 `main/jobs` 历史只读桥。绝不修改旧格式；每条结果都标记 legacy_unverified，
    // 且不返回本机绝对路径（旧 manifest 的 parameters 永不外泄）。
    impl_->server->Get(R"(/api/v1/projects/([^/]+)/legacy/jobs)",
                       [this, config, send](const httplib::Request& req, httplib::Response& res) {
        if (!PassesBoundary(req, config, res, send)) return;
        if (!IsAuthorized(req, config.token)) {
            res.status = 401;
            send(res, FailureEnvelope(MakeRequestId(), MakeTraceId(), "UNAUTHENTICATED",
                                      "missing or invalid bearer token", false));
            return;
        }
        const std::string projectId = req.matches.size() > 1 ? req.matches[1].str() : "";
        std::filesystem::path projectRoot;
        {
            std::lock_guard<std::mutex> lock(impl_->projectStateMutex);
            const auto it = impl_->projectStates.find(projectId);
            if (it == impl_->projectStates.end()) {
                res.status = 404;
                send(res, FailureEnvelope(MakeRequestId(), MakeTraceId(), "NOT_FOUND",
                                          "unknown project", false));
                return;
            }
            projectRoot = it->second.request.projectRoot;
        }
        LegacyJobBridge bridge(projectRoot);
        std::vector<LegacyJobEntry> entries;
        std::string error;
        if (!bridge.List(entries, error)) {
            res.status = 503;
            send(res, FailureEnvelope(MakeRequestId(), MakeTraceId(), "SERVICE_UNAVAILABLE",
                                      error.empty() ? "legacy history is not readable" : error,
                                      true));
            return;
        }
        std::size_t limit = LegacyJobBridge::kDefaultLimit;
        std::size_t offset = 0;
        if (req.has_param("limit")) {
            try {
                limit = static_cast<std::size_t>(std::stoull(req.get_param_value("limit")));
            } catch (const std::exception&) {
                limit = LegacyJobBridge::kDefaultLimit;
            }
        }
        if (req.has_param("cursor")) {
            try {
                offset = static_cast<std::size_t>(std::stoull(req.get_param_value("cursor")));
            } catch (const std::exception&) {
                offset = 0;
            }
        }
        limit = std::min<std::size_t>(std::max<std::size_t>(limit, 1),
                                      LegacyJobBridge::kMaxLimit);
        Json jobs = Json::array();
        const std::size_t begin = std::min(offset, entries.size());
        const std::size_t end = std::min(entries.size(), begin + limit);
        for (std::size_t i = begin; i < end; ++i) {
            jobs.push_back(LegacyJobBridge::ToJson(entries[i]));
        }
        Json data;
        data["project_id"] = projectId;
        data["origin"] = "legacy";
        data["completeness"] = "legacy_unverified";
        data["jobs"] = jobs;
        data["total"] = entries.size();
        data["offset"] = begin;
        data["limit"] = limit;
        data["has_more"] = end < entries.size();
        data["next_cursor"] = end < entries.size() ? Json(std::to_string(end)) : Json(nullptr);
        res.status = 200;
        send(res, SuccessEnvelope(MakeRequestId(), MakeTraceId(), data));
    });

    impl_->server->Get(R"(/api/v1/projects/([^/]+)/legacy/jobs/([^/]+)/report)",
                       [this, config, send](const httplib::Request& req, httplib::Response& res) {
        if (!PassesBoundary(req, config, res, send)) return;
        if (!IsAuthorized(req, config.token)) {
            res.status = 401;
            send(res, FailureEnvelope(MakeRequestId(), MakeTraceId(), "UNAUTHENTICATED",
                                      "missing or invalid bearer token", false));
            return;
        }
        const std::string projectId = req.matches.size() > 1 ? req.matches[1].str() : "";
        const std::string jobId = req.matches.size() > 2 ? req.matches[2].str() : "";
        std::filesystem::path projectRoot;
        {
            std::lock_guard<std::mutex> lock(impl_->projectStateMutex);
            const auto it = impl_->projectStates.find(projectId);
            if (it == impl_->projectStates.end()) {
                res.status = 404;
                send(res, FailureEnvelope(MakeRequestId(), MakeTraceId(), "NOT_FOUND",
                                          "unknown project", false));
                return;
            }
            projectRoot = it->second.request.projectRoot;
        }
        LegacyJobBridge bridge(projectRoot);
        Json normalized;
        std::string error;
        if (!bridge.ReadReport(jobId, projectId, normalized, error)) {
            res.status = 404;
            send(res, FailureEnvelope(MakeRequestId(), MakeTraceId(), "NOT_FOUND",
                                      "legacy report is not available: " + error, false));
            return;
        }
        res.status = 200;
        send(res, SuccessEnvelope(MakeRequestId(), MakeTraceId(), normalized));
    });

    // 未知路径/方法/超限统一返回失败信封。
    impl_->server->set_error_handler([send](const httplib::Request&,
                                            httplib::Response& res) {
        if (!res.body.empty()) return;  // 已有应答
        std::string code = "INVALID_ARGUMENT";
        bool retryable = false;
        switch (res.status) {
            case 400: code = "INVALID_ARGUMENT"; break;
            case 404: code = "NOT_FOUND"; break;
            case 405: code = "INVALID_ARGUMENT"; break;   // method not allowed
            case 413: code = "RESOURCE_EXHAUSTED"; break; // payload too large
            case 415: code = "INVALID_ARGUMENT"; break;
            case 429: code = "RESOURCE_EXHAUSTED"; retryable = true; break;
            case 503: code = "SERVICE_UNAVAILABLE"; retryable = true; break;
            default: break;
        }
        send(res, FailureEnvelope(MakeRequestId(), MakeTraceId(), code,
                                  "status " + std::to_string(res.status), retryable));
    });

    // SF-02：优先显式端口（便于端口占用故障测试与固定配置），否则系统分配。
    if (config.port > 0) {
        if (!impl_->server->bind_to_port(config.bindAddress, config.port)) {
            error = "failed to bind " + config.bindAddress + ":" + std::to_string(config.port);
            impl_->server.reset();
            return false;
        }
        impl_->boundPort = config.port;
    } else {
        impl_->boundPort = impl_->server->bind_to_any_port(config.bindAddress);
        if (impl_->boundPort <= 0) {
            error = "failed to bind " + config.bindAddress + " (any port)";
            impl_->server.reset();
            return false;
        }
    }

    impl_->running.store(true);
    return true;
}

void GatewayServer::RunAsync() {
    if (!impl_->running.load() || !impl_->server) return;
    impl_->worker = std::thread([this]() { impl_->server->listen_after_bind(); });
}

void GatewayServer::Stop() {
    if (impl_->server) {
        impl_->server->stop();
    }
    if (impl_->worker.joinable()) {
        impl_->worker.join();
    }
    impl_->running.store(false);
    impl_->server.reset();
}

int GatewayServer::Port() const { return impl_->boundPort; }

bool GatewayServer::Running() const { return impl_->running.load(); }

bool GatewayServer::ConfigureGrantPersistence(const std::string& path, std::string& error) {
    if (!impl_) {
        error = "gateway is unavailable";
        return false;
    }
    // receipt 索引与 grant 索引同目录（同名不同后缀），随工程切换一起失效。
    std::filesystem::path receiptPath;
    if (!path.empty()) {
        receiptPath = platform::PathFromUtf8(path).parent_path() / "ui-receipts.json";
    }
    if (!impl_->grants.ConfigurePersistence(platform::PathFromUtf8(path), error)) return false;
    return impl_->receipts.ConfigurePersistence(receiptPath, error);
}

bool GatewayServer::UpdateProjectSnapshotState(const ProjectSnapshotState& state,
                                               std::string& error) {
    error.clear();
    if (!impl_ || state.request.projectId.empty() || state.request.projectRoot.empty() ||
        state.request.currentRevision.empty() || !state.sourceSummary.is_array()) {
        error = "project snapshot state is incomplete";
        return false;
    }
    ProjectSnapshotState copy = state;
    copy.request.expectedRevision = copy.request.currentRevision;
    const std::string projectId = copy.request.projectId;
    {
        std::lock_guard<std::mutex> lock(impl_->projectStateMutex);
        impl_->projectStates[projectId] = std::move(copy);
    }

    // CoreJobService restores persisted manifests before Gateway is started.
    // Rebuild the Gateway-only ownership index from service-owned provenance,
    // rather than trusting a separate mutable UI cache.  This also restores
    // completed idempotency associations and prevents a retry after restart
    // from silently creating a second Job.
    IJobService* service = impl_->jobServiceProvider ? impl_->jobServiceProvider() : nullptr;
    if (service != nullptr) {
        for (const JobRecord& record : service->list(projectId)) {
            const Json& metadata = record.request.metadata;
            if (!metadata.is_object() ||
                metadata.value("schema", std::string()) != "sigflow.edu-agent-job-binding.v1" ||
                metadata.value("project_id", std::string()) != projectId || record.id.empty()) {
                continue;
            }
            const std::string snapshotId = metadata.value("snapshot_id", std::string());
            const std::string revision = metadata.value("revision", std::string());
            const std::string capability = metadata.value("capability", std::string());
            if (snapshotId.empty() || revision.empty() || capability.empty()) continue;
            Json binding{{"project_id", projectId},
                         {"snapshot_id", snapshotId},
                         {"revision", revision},
                         {"capability", capability},
                         {"job_type", record.request.jobType},
                         {"plan_hash", metadata.value("plan_hash", std::string())},
                         {"step_id", metadata.value("step_id", std::string())},
                         {"state", JobStateName(record.state)}};
            {
                std::lock_guard<std::mutex> lock(impl_->jobMutex);
                impl_->jobProjects[record.id] = projectId;
                impl_->jobBindings[record.id] = std::move(binding);
            }
            impl_->jobEvents.Track(record.id, projectId, capability, snapshotId, revision);
            const std::string scope = metadata.value("idempotency_scope", std::string());
            const std::string key = metadata.value("idempotency_key", std::string());
            const std::string hash = metadata.value("request_hash", std::string());
            if (!scope.empty() && !key.empty() && !hash.empty()) {
                impl_->idempotency.RestoreCompleted(scope, key, hash, record.id);
            }
        }
    }
    return true;
}

bool GatewayServer::RefreshProjectSnapshotStateFromDisk(
    const std::filesystem::path& projectRoot, bool dirty, bool synchronized,
    std::string& projectId, std::string& error) {
    projectId.clear();
    error.clear();
    if (!impl_ || projectRoot.empty()) {
        error = "project root is required";
        return false;
    }

    std::error_code ec;
    const std::filesystem::path canonicalRoot =
        std::filesystem::weakly_canonical(projectRoot, ec);
    if (ec || !std::filesystem::is_directory(canonicalRoot, ec) || ec) {
        error = "project root is unavailable";
        return false;
    }

    JsonProject project;
    if (!project.Load(canonicalRoot / "sigflow.project", error)) return false;

    const std::string rootIdentity = platform::PathToUtf8(canonicalRoot);
    projectId = "project-" +
        platform::Sha256Hex(rootIdentity.data(), rootIdentity.size()).substr(0, 24);

    struct SourceDescription {
        SnapshotSourceInput input;
        Json summary;
        std::string sortKey;
    };
    std::vector<SourceDescription> descriptions;
    std::set<std::string> registeredPaths;
    for (const std::filesystem::path& configured : project.SourceFiles()) {
        if (configured.empty() || configured == "." || configured.is_absolute() ||
            configured.has_root_name() || configured.has_root_directory()) {
            error = "project source path must be a non-empty relative path";
            return false;
        }
        const std::filesystem::path normalized = configured.lexically_normal();
        if (normalized.empty() || normalized == "." ||
            (normalized.begin() != normalized.end() && *normalized.begin() == "..")) {
            error = "project source path escapes the project root";
            return false;
        }
        const std::filesystem::path absolute =
            std::filesystem::weakly_canonical(canonicalRoot / normalized, ec);
        if (ec || !IsWithinPath(absolute, canonicalRoot)) {
            error = "project source resolves outside the project root";
            return false;
        }
        if (!std::filesystem::is_regular_file(absolute, ec) || ec) {
            error = "registered project source is unavailable";
            return false;
        }
        const std::filesystem::path relative = absolute.lexically_relative(canonicalRoot);
        // 契约路径 = UTF-8 + POSIX 分隔符；不要用 generic_string()（MinGW 下双重编码）。
        const std::string relativeUtf8 = platform::RelativePathToUtf8(relative);
        if (!registeredPaths.insert(relativeUtf8).second) {
            error = "project contains duplicate source paths";
            return false;
        }
        const std::string sha256 = platform::Sha256FileHex(absolute);
        if (sha256.empty()) {
            error = "unable to hash registered project source";
            return false;
        }
        const std::uint64_t size = std::filesystem::file_size(absolute, ec);
        if (ec) {
            error = "unable to read registered project source size";
            return false;
        }
        const std::string sourceId = "source-" +
            platform::Sha256Hex(relativeUtf8.data(), relativeUtf8.size()).substr(0, 24);
        SourceDescription description;
        description.input.sourceId = sourceId;
        description.input.relativePath = relative;
        description.input.expectedSha256 = sha256;
        description.summary = Json{{"source_id", sourceId},
                                   {"path", relativeUtf8},
                                   {"sha256", sha256},
                                   {"size", std::to_string(size)},
                                   {"origin", "project"}};
        description.sortKey = relativeUtf8;
        descriptions.push_back(std::move(description));
    }
    std::sort(descriptions.begin(), descriptions.end(),
              [](const SourceDescription& left, const SourceDescription& right) {
                  return left.sortKey < right.sortKey;
              });

    Json target = Json::object();
    Json toolConfig = Json::object();
    const Json& document = project.Document();
    if (document.contains("fpga") && document["fpga"].is_object()) {
        const Json& fpga = document["fpga"];
        if (fpga.contains("target_profile") && fpga["target_profile"].is_string()) {
            target["profile"] = fpga["target_profile"];
        }
        if (fpga.contains("yosys_strategy") && fpga["yosys_strategy"].is_string()) {
            toolConfig["yosys_strategy"] = fpga["yosys_strategy"];
        }
    }
    const std::string manifestHash =
        platform::Sha256FileHex(canonicalRoot / "sigflow.project");
    if (manifestHash.empty()) {
        error = "unable to hash project manifest";
        return false;
    }
    toolConfig["project_manifest_sha256"] = manifestHash;

    // SF-05：外部 include 白名单——仅登记工程内外都显式声明的目录，且必须落在工程根内。
    // 未登记的 include 目录一律不进入快照（Spec §6.1：不允许 Agent 读工程外任意路径）。
    std::vector<std::filesystem::path> allowedExternalRoots;
    if (document.contains("paths") && document["paths"].is_object() &&
        document["paths"].contains("include_dirs") &&
        document["paths"]["include_dirs"].is_array()) {
        for (const auto& entry : document["paths"]["include_dirs"]) {
            if (!entry.is_string()) continue;
            const std::filesystem::path configured(entry.get<std::string>());
            if (configured.empty() || configured.is_absolute() ||
                configured.has_root_name() || configured.has_root_directory()) {
                error = "include_dirs entries must be relative to the project root";
                return false;
            }
            const std::filesystem::path normalized = configured.lexically_normal();
            if (normalized.empty() || normalized == "." ||
                (normalized.begin() != normalized.end() && *normalized.begin() == "..")) {
                error = "include_dirs entry escapes the project root";
                return false;
            }
            const std::filesystem::path absolute =
                std::filesystem::weakly_canonical(canonicalRoot / normalized, ec);
            if (ec || !IsWithinPath(absolute, canonicalRoot) ||
                !std::filesystem::is_directory(absolute, ec) || ec) {
                error = "include_dirs entry resolves outside the project root or is not a directory";
                return false;
            }
            allowedExternalRoots.push_back(absolute);
        }
    }

    Json sourceSummary = Json::array();
    Json fingerprintSources = Json::array();
    std::vector<SnapshotSourceInput> inputs;
    inputs.reserve(descriptions.size());
    for (SourceDescription& description : descriptions) {
        inputs.push_back(std::move(description.input));
        sourceSummary.push_back(description.summary);
        fingerprintSources.push_back(description.summary);
    }
    const Json fingerprintDocument{{"project_id", projectId},
                                   {"top", project.TopModule()},
                                   {"sources", fingerprintSources},
                                   {"target", target},
                                   {"tool_config", toolConfig}};
    const std::string fingerprint = IdempotencyStore::HashRequest(fingerprintDocument);
    std::string revision;
    if (!ObserveRevision(canonicalRoot, projectId, fingerprint, revision, error)) return false;

    ProjectSnapshotState state;
    state.request.projectId = projectId;
    state.request.projectRoot = canonicalRoot;
    state.request.expectedRevision = revision;
    state.request.currentRevision = revision;
    state.request.dirty = dirty;
    state.request.synchronized = synchronized;
    state.request.top = project.TopModule();
    state.request.sources = std::move(inputs);
    state.request.expectedProjectManifestSha256 = manifestHash;
    state.request.allowedExternalRoots = std::move(allowedExternalRoots);
    state.request.target = std::move(target);
    state.request.toolConfig = std::move(toolConfig);
    state.sourceSummary = std::move(sourceSummary);
    state.policyVersion = "edu-policy-v1";
    return UpdateProjectSnapshotState(state, error);
}

bool GatewayServer::MarkProjectDirty(const std::string& projectId, bool dirty,
                                     const std::string& bufferHash) {
    if (!impl_ || projectId.empty()) return false;
    {
        std::lock_guard<std::mutex> lock(impl_->projectStateMutex);
        const auto it = impl_->projectStates.find(projectId);
        if (it == impl_->projectStates.end()) return false;
        it->second.request.dirty = dirty;
        // buffer_hash 只进入 context 摘要；不能用于正式 snapshot。
        it->second.bufferHash = bufferHash;
    }
    // NG-08：只有 dirty 状态真正翻转时才投递事件，避免每次按键都产生噪声事件。
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(impl_->eventSourceMutex);
        const auto it = impl_->lastDirtyState.find(projectId);
        changed = it == impl_->lastDirtyState.end() || it->second != dirty;
        impl_->lastDirtyState[projectId] = dirty;
    }
    if (changed) {
        PublishEvent("project/dirty", projectId, MakeTraceId(),
                     Json{{"project_id", projectId},
                          {"dirty", dirty},
                          {"buffer_hash", bufferHash.empty() ? Json(nullptr)
                                                             : Json(bufferHash)}});
    }
    return true;
}

void GatewayServer::RemoveProjectSnapshotState(const std::string& projectId) {
    if (!impl_) return;
    {
        std::lock_guard<std::mutex> lock(impl_->projectStateMutex);
        impl_->projectStates.erase(projectId);
    }
    impl_->waveformService.RemoveProject(projectId);
    impl_->artifactService.RemoveProject(projectId);
    // NG-05：切工程/关工程时一并丢弃该工程的局部设计映射与缓冲片段。
    impl_->designContext.ClearProject(projectId);
}

void GatewayServer::PublishEvent(const std::string& type, const std::string& projectId,
                                 const std::string& traceId, const eda::Json& data) {
    if (!impl_) return;
    const Json event = impl_->events.Append(type, projectId, traceId, data);
    // 事件以 JSONL 追加落盘（重启可重放）；失败不阻断。
    if (!impl_->config.eventLogPath.empty()) {
        impl_->events.AppendToFile(impl_->config.eventLogPath, event);
    }
}

void GatewayServer::NotifyProjectOpened(const std::string& projectId) {
    if (!impl_ || projectId.empty()) return;
    Json data{{"project_id", projectId}};
    std::string revision;
    {
        std::lock_guard<std::mutex> lock(impl_->projectStateMutex);
        const auto it = impl_->projectStates.find(projectId);
        if (it != impl_->projectStates.end()) revision = it->second.request.currentRevision;
    }
    if (!revision.empty()) data["revision"] = revision;
    PublishEvent("project/opened", projectId, MakeTraceId(), data);
}

void GatewayServer::NotifyProjectSaved(const std::string& projectId) {
    if (!impl_ || projectId.empty()) return;
    std::string revision;
    {
        std::lock_guard<std::mutex> lock(impl_->projectStateMutex);
        const auto it = impl_->projectStates.find(projectId);
        if (it != impl_->projectStates.end()) revision = it->second.request.currentRevision;
    }
    PublishEvent("project/saved", projectId, MakeTraceId(),
                 Json{{"project_id", projectId},
                      {"revision", revision.empty() ? Json(nullptr) : Json(revision)}});
    if (revision.empty()) return;
    bool advanced = false;
    {
        std::lock_guard<std::mutex> lock(impl_->eventSourceMutex);
        const auto it = impl_->lastSavedRevision.find(projectId);
        advanced = it == impl_->lastSavedRevision.end() || it->second != revision;
        impl_->lastSavedRevision[projectId] = revision;
    }
    if (advanced) {
        PublishEvent("revision/advanced", projectId, MakeTraceId(),
                     Json{{"project_id", projectId}, {"revision", revision}});
    }
}

void GatewayServer::NotifyProjectClosed(const std::string& projectId) {
    if (!impl_ || projectId.empty()) return;
    PublishEvent("project/closed", projectId, MakeTraceId(), Json{{"project_id", projectId}});
    {
        std::lock_guard<std::mutex> lock(impl_->eventSourceMutex);
        impl_->lastSavedRevision.erase(projectId);
        impl_->lastSelectionState.erase(projectId);
        impl_->lastDirtyState.erase(projectId);
    }
    RemoveProjectSnapshotState(projectId);
}

GatewayServer::RetentionResult GatewayServer::RunSnapshotRetention(std::size_t keepRecent,
                                                                  std::uint64_t maxAgeSeconds,
                                                                  std::uint64_t minIntervalSeconds,
                                                                  std::uint64_t nowEpoch) {
    RetentionResult result;
    if (!impl_) return result;
    const auto now = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lock(impl_->eventSourceMutex);
        if (impl_->retentionRanOnce && minIntervalSeconds > 0 &&
            now - impl_->lastRetentionRun < std::chrono::seconds(minIntervalSeconds)) {
            result.skippedByInterval = true;
            return result;
        }
        impl_->retentionRanOnce = true;
        impl_->lastRetentionRun = now;
    }
    std::vector<std::string> projectIds;
    {
        std::lock_guard<std::mutex> lock(impl_->projectStateMutex);
        for (const auto& entry : impl_->projectStates) projectIds.push_back(entry.first);
    }
    for (const std::string& projectId : projectIds) {
        result.projectsConsidered += 1;
        std::size_t removed = 0;
        std::string error;
        Json entry{{"project_id", projectId}};
        if (PruneProjectSnapshots(projectId, keepRecent, maxAgeSeconds, removed, error, nowEpoch)) {
            entry["removed"] = removed;
            entry["status"] = "ok";
            result.snapshotsRemoved += removed;
        } else {
            // 清理失败不影响其它工程；明确记录原因，不假装成功。
            entry["removed"] = 0;
            entry["status"] = "failed";
            entry["error"] = error;
        }
        result.report.push_back(std::move(entry));
    }
    return result;
}

// SF-02：把已登记 Agent Job 的状态变化转成事件。宿主按固定间隔调用即可（幂等）。
std::size_t GatewayServer::PollJobEvents() {
    if (!impl_) return 0;
    IJobService* service = impl_->jobServiceProvider ? impl_->jobServiceProvider() : nullptr;
    if (service == nullptr) return 0;

    const std::vector<JobEventSource::Emitted> emitted = impl_->jobEvents.Poll(service);
    std::size_t published = 0;
    for (const auto& change : emitted) {
        Json data{{"job_id", change.jobId},
                  {"state", change.state},
                  {"exit_code", change.exitCode}};
        if (!change.capability.empty()) data["capability"] = change.capability;
        if (!change.snapshotId.empty()) data["snapshot_id"] = change.snapshotId;
        if (!change.revision.empty()) data["revision"] = change.revision;
        PublishEvent(change.type, change.projectId, MakeTraceId(), data);
        ++published;
    }
    return published;
}

} // namespace agent
} // namespace eda
