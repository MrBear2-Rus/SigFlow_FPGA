#include "Composer.h"

#include "ISigPlugin.h"
#include "PluginManager.h"

#include <eda/api/process.hpp>

#include <wx/config.h>
#include <wx/string.h>

#include <cstdlib>
#include <sstream>

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

Composer::Composer()
    : jobService_(std::make_unique<eda::CoreJobService>(
          []() { return eda::CreatePlatformProcessHost(); })) {}
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
