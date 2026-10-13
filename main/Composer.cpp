#include "Composer.h"

#include "ISigPlugin.h"
#include "PluginManager.h"
#include "eda-core/Toolchain.h"
#include "VerilatorSimulator.h"

#include <eda/api/process.hpp>
#include <eda/api/toolchain.hpp>
#include <eda-platform/Platform.h>

#include <wx/config.h>
#include <wx/string.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <sstream>
#include <chrono>
#include <map>

namespace sigflow {

// P1-8 修复：静态库中的自注册 TU 若无外部引用会被链接器丢弃，导致 RegisterBuiltins 为空。
// 显式调用各官方插件注册宏生成的锚点函数，把注册对象拉入最终可执行。
// 新增官方插件时，在此追加对应锚点（与 EDA_REGISTER_PLUGIN 的 UniqueName 一致）。
extern "C" void eda_plugin_force_link_YosysSynthesizer();
extern "C" void eda_plugin_force_link_NextpnrPlaceRouter();
extern "C" void eda_plugin_force_link_GowinPacker();
extern "C" void eda_plugin_force_link_OpenFpgaLoaderProgrammer();
extern "C" void eda_plugin_force_link_VerilatorSimulator();
extern "C" void eda_plugin_force_link_VerilatorRunJob();

namespace {
void ForceLinkOfficialPlugins() {
    eda_plugin_force_link_YosysSynthesizer();
    eda_plugin_force_link_NextpnrPlaceRouter();
    eda_plugin_force_link_GowinPacker();
    eda_plugin_force_link_OpenFpgaLoaderProgrammer();
    eda_plugin_force_link_VerilatorSimulator();
    eda_plugin_force_link_VerilatorRunJob();
}
} // namespace

Composer::Composer() {
    std::filesystem::path root;
#if defined(SIGFLOW_BUILD_EDU_AGENT)
    // 教育版在打开工程后由 ConfigureJobStorage 切到工程目录。
#else
    root = eda::CoreJobService::ResolveJobRoot({}, eda::platform::AppDataRoot(), {},
                                               jobRootWarning_);
#endif
    jobService_ = std::make_unique<eda::CoreJobService>(
        []() { return eda::CreatePlatformProcessHost(); }, root);
}
Composer::~Composer() = default;

std::string Composer::LoadPlugins(const std::string& pluginDirectoryUtf8,
                                  const std::string& exeDirUtf8,
                                  const std::string& projectDirUtf8) {
    std::ostringstream report;

    // 1) legacy ISigPlugin（DeepSeek 等，P3 再迁移到新契约）。
    legacyManager_ = std::make_unique<PluginManager>();
    legacyManager_->LoadPlugins(pluginDirectoryUtf8);
    for (const auto& name : LegacyPluginNames()) {
        report << name << " Loaded\n";
    }

    // 2) 新契约 IPluginInteraction：内置注册表 + 搜索链（bundled/用户/项目/env）发现。
    pluginHost_.AddSearchPath(pluginDirectoryUtf8);
    if (!exeDirUtf8.empty() || !projectDirUtf8.empty()) {
        pluginHost_.AddDefaultSearchPaths(exeDirUtf8, projectDirUtf8);
    }
    // 先把静态自注册 TU 拉入链接（否则静态库按需拉取会丢弃注册对象）。
    ForceLinkOfficialPlugins();
    pluginHost_.RegisterBuiltins();
    pluginHost_.Discover();
    pluginHost_.LoadAll();
    report << pluginHost_.DiagnosticReport();

    // P1-8：把内置插件中实现 IJobProvider 的实例注册到核心 Job 服务，供流程按钮/能力选择器使用。
    for (const auto& record : pluginHost_.Records()) {
        if (record.status != eda::PluginStatus::Ready) continue;
        eda::IPluginInteraction* instance = pluginHost_.Get(record.id);
        if (instance == nullptr) continue;
        if (auto* provider = dynamic_cast<eda::IJobProvider*>(instance)) {
            jobService_->RegisterProvider(std::shared_ptr<eda::IJobProvider>(provider, [](eda::IJobProvider*) {}));
        }
    }
    return report.str();
}

eda::IJobService& Composer::JobService() { return *jobService_; }

void Composer::RegisterGuiJobProvider(std::shared_ptr<eda::IJobProvider> provider) {
    if (!provider || !jobService_) return;
    jobService_->RegisterProvider(provider);
    guiJobProviders_.push_back(std::move(provider));
}

// SF-03：把 Job 数据根目录切到受控位置。
// 拒绝在已有 Job 记录/providers 时迁移——运行中迁移会让记录分落两处，恢复语义无法保证。
bool Composer::ConfigureJobStorage(const std::string& projectAgentRootUtf8,
                                   const std::string& projectKey) {
    if (jobService_ == nullptr) return false;
    if (!jobService_->list(std::string()).empty()) return false;
    const std::filesystem::path projectAgentRoot =
        projectAgentRootUtf8.empty() ? std::filesystem::path()
                                     : eda::platform::PathFromUtf8(projectAgentRootUtf8);
    const std::filesystem::path appData = eda::platform::AppDataRoot();
    std::string warning;
    const std::filesystem::path root = eda::CoreJobService::ResolveJobRoot(
        projectAgentRoot, appData, projectKey, warning);

    auto replacement = std::make_unique<eda::CoreJobService>(
        []() { return eda::CreatePlatformProcessHost(); }, root);
    // 保留已注册的 provider（切换根目录不应丢失插件能力）。
    for (const auto& record : pluginHost_.Records()) {
        if (record.status != eda::PluginStatus::Ready) continue;
        eda::IPluginInteraction* instance = pluginHost_.Get(record.id);
        if (instance == nullptr) continue;
        if (auto* provider = dynamic_cast<eda::IJobProvider*>(instance)) {
            replacement->RegisterProvider(
                std::shared_ptr<eda::IJobProvider>(provider, [](eda::IJobProvider*) {}));
        }
    }
    for (const auto& provider : guiJobProviders_) replacement->RegisterProvider(provider);
    replacement->SetLogSink(nullptr);
    jobService_ = std::move(replacement);
    jobRootWarning_ = warning;
    return true;
}

std::string Composer::JobServiceRoot() const {
    if (jobService_ == nullptr) return {};
    return eda::platform::PathToUtf8(jobService_->JobRoot());
}

std::vector<std::string> Composer::Capabilities() const {
    std::vector<std::string> capabilities;
    for (const auto& record : pluginHost_.Records()) {
        if (record.status != eda::PluginStatus::Ready) continue;
        for (const auto& capability : record.capabilities) {
            capabilities.push_back(capability);
        }
    }
    return capabilities;
}

std::vector<eda::PluginInfo> Composer::Providers(const std::string& capability) const {
    return pluginHost_.ProvidersOf(capability);
}

std::vector<Composer::ReadyPluginInfo> Composer::ReadyPlugins() const {
    std::vector<ReadyPluginInfo> ready;
    for (const auto& record : pluginHost_.Records()) {
        if (record.status != eda::PluginStatus::Ready) continue;
        ReadyPluginInfo info;
        info.id = record.id;
        info.version = record.version;
        // 教育能力的判定依据必须是**真实 Job 执行体**（IJobProvider::jobType），
        // 而不是插件信息里的装饰性 capability 串（如 "synth/yosys"）——后者与
        // 教育白名单 "synth"/"sim.build"/"sim.run" 不同名，会导致能力被误判为缺失。
        info.capabilities = record.capabilities;
        if (eda::IPluginInteraction* instance = pluginHost_.Get(record.id)) {
            if (auto* provider = dynamic_cast<eda::IJobProvider*>(instance)) {
                const std::string jobType = provider->jobType();
                if (!jobType.empty() &&
                    std::find(info.capabilities.begin(), info.capabilities.end(), jobType) ==
                        info.capabilities.end()) {
                    info.capabilities.push_back(jobType);
                }
                // AD-12：插件在册 ≠ 现在能跑。工具链缺失时把该插件标记为不可用，
                // 让 Gateway 的能力列表给出 ready=false + 可读原因（Agent 据此拒绝计划）。
                std::string toolReason;
                if (!ToolAvailableForJobType(jobType, toolReason)) {
                    info.ready = false;
                    info.reason = toolReason;
                }
            }
        }
        ready.push_back(std::move(info));
    }
    return ready;
}

void Composer::SetAgentToolConfig(const eda::Json& config) {
    std::lock_guard<std::mutex> lock(toolConfigMutex_);
    agentToolConfig_ = config;
}

bool Composer::ToolAvailableForJobType(const std::string& jobType, std::string& reason) const {
    // 只覆盖教育版三项能力需要的工具；其它 jobType 不做猜测（返回可用）。
    eda::ToolQuery query;
    if (jobType == "synth") {
        query.name = "yosys";
        query.environmentVariables = {"SIGFLOW_YOSYS"};
    } else if (jobType == "sim.build" || jobType == "sim.run") {
        query.name = "verilator_bin";
        query.alternativeNames = {"verilator_bin_dbg", "verilator"};
        query.environmentVariables = {"SIGFLOW_VERILATOR", "VERILATOR_BIN"};
    } else {
        return true;
    }
    query.searchPath = true;
    {
        std::lock_guard<std::mutex> lock(toolConfigMutex_);
        const char* name = jobType == "synth" ? "yosys_path" : "verilator_path";
        if (agentToolConfig_.contains(name) && agentToolConfig_[name].is_string()) query.configuredPath = agentToolConfig_[name];
    }
    const eda::ToolResolution resolution = eda::DefaultToolchain().Resolve(query);
    if (resolution.found) {
        // Version probes run in Gateway workers, not on the GUI thread. Cache
        // briefly to avoid a process storm when capability discovery is polled.
        static std::mutex cacheMutex;
        static std::map<std::string, std::tuple<std::chrono::steady_clock::time_point, bool, std::string>> cache;
        const std::string key = eda::platform::PathToUtf8(resolution.path) + jobType + eda::platform::EnvUtf8("PATH") +
            eda::platform::EnvUtf8("SIGFLOW_SH") + eda::platform::EnvUtf8("SIGFLOW_PYTHON");
        std::lock_guard<std::mutex> lock(cacheMutex);
        const auto now = std::chrono::steady_clock::now();
        const auto prior = cache.find(key);
        if (prior != cache.end() && now - std::get<0>(prior->second) < std::chrono::seconds(10)) {
            reason = std::get<2>(prior->second); return std::get<1>(prior->second);
        }
        bool ready = false;
        if (jobType == "synth") {
            auto host = eda::CreatePlatformProcessHost();
            eda::ProcessSpec spec; spec.executable = resolution.path; spec.arguments = {"-V"};
            spec.timeoutSeconds = 1; spec.maxOutputBytes = 8192;
            ready = host->Run(spec, {}).outcome == eda::ProcessOutcome::Success;
            reason = ready ? "" : "yosys version probe failed";
        } else ready = eda::sim::CheckVerilatorToolchain(resolution.path, reason);
        if (cache.size() >= 32) cache.clear();
        cache[key] = {now, ready, reason}; return ready;
    }
    reason = "tool for '" + jobType + "' is not available: " +
             (resolution.reason.empty() ? query.name + " was not found" : resolution.reason);
    return false;
}

eda::PluginInfo Composer::DefaultProvider(const std::string& capability,
                                          const std::string& preferredId) const {
    // 优先级：显式 preferredId → 用户持久化的默认后端 → 该能力首个就绪后端。
    std::string wanted = preferredId;
    if (wanted.empty()) wanted = StoredDefaultProvider(capability);
    if (!wanted.empty()) {
        for (const auto& info : pluginHost_.ProvidersOf(capability)) {
            if (info.id == wanted) return info;
        }
    }
    const auto providers = pluginHost_.ProvidersOf(capability);
    return providers.empty() ? eda::PluginInfo{} : providers.front();
}

bool Composer::UseJobService() {
    const char* value = std::getenv("SIGFLOW_USE_JOB_SERVICE");
    return value != nullptr && value[0] == '1';
}

std::string Composer::SubmitJob(const std::string& jobType, const std::string& projectId,
                                const eda::Json& params, bool requireConfirm,
                                std::string& error) {
    if (!jobService_) {
        error = "job service is not available";
        return {};
    }
    eda::JobRequest request;
    request.jobType = jobType;
    request.projectId = projectId;
    request.params = params;
    request.requireConfirm = requireConfirm;
    const std::string id = jobService_->submit(request);
    if (id.empty()) {
        error = "job submission failed: " + jobType;
        return {};
    }
    return id;
}

// P1-8：默认后端选择持久化（键：Toolchain/DefaultBackend/<capability>）。
static wxString DefaultBackendConfigKey(const std::string& capability) {
    return wxString::Format("Toolchain/DefaultBackend/%s",
                            wxString::FromUTF8(capability.c_str()));
}

std::string Composer::StoredDefaultProvider(const std::string& capability) {
    wxConfigBase* config = wxConfigBase::Get(false);
    if (config == nullptr) return {};
    wxString value;
    if (!config->Read(DefaultBackendConfigKey(capability), &value)) return {};
    return std::string(value.ToUTF8().data());
}

void Composer::StoreDefaultProvider(const std::string& capability, const std::string& pluginId) {
    wxConfigBase* config = wxConfigBase::Get(false);
    if (config == nullptr) return;
    config->Write(DefaultBackendConfigKey(capability),
                  wxString::FromUTF8(pluginId.c_str()));
    config->Flush();
}

ISigPlugin* Composer::LegacyPlugin(const std::string& name) const {
    return legacyManager_ ? legacyManager_->GetPlugin(name) : nullptr;
}

// P2-8：legacy 插件名集中在此处，宿主不再散落硬编码。
ISigPlugin* Composer::LegacyAssistant() const {
    static const char* const kAssistantPluginName = "DeepSeek_Assistant";
    return LegacyPlugin(kAssistantPluginName);
}

std::vector<std::string> Composer::LegacyPluginNames() const {
    std::vector<std::string> names;
    if (!legacyManager_) return names;
    for (auto* plugin : legacyManager_->GetAllPlugins()) {
        if (plugin) names.push_back(plugin->GetName());
    }
    return names;
}

std::vector<std::string> Composer::PluginNames() const {
    std::vector<std::string> names;
    for (const auto& record : pluginHost_.Records()) {
        if (record.status == eda::PluginStatus::Ready) names.push_back(record.id);
    }
    return names;
}

} // namespace sigflow
