#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <eda/api/IPluginInteraction.h>
#include <eda/api/services.hpp>

namespace eda {
namespace platform {
class DynamicLibrary;
} // namespace platform

enum class PluginStatus { Discovered, Validated, Resolved, Loaded, Ready, Error };

const char* ToString(PluginStatus status);

struct PluginRecord {
    std::string id;
    std::string version;
    PluginStatus status = PluginStatus::Discovered;
    std::filesystem::path path;   // 动态库路径；静态插件为空
    std::string location;         // "inner" | "external"
    std::string runtime;          // "inprocess" | "process"
    std::vector<std::string> capabilities;
    std::string error;            // Error 时的结构化原因（绝不静默，修 R1）
    bool isStatic = false;
};

// 插件宿主：发现 → 校验（manifest/ABI/构建指纹）→ 注册 → 激活，失败必留诊断。
// 修 R1（失败不再静默）与 R2（按能力选择，不再按名字硬编码）。
class PluginHost {
public:
    PluginHost();
    ~PluginHost();

    // 发现链（顺序即优先级）。
    void AddSearchPath(const std::filesystem::path& directory);
    void AddSearchPathsFromEnvironment();   // SIGFLOW_PLUGIN_PATH（平台分隔符切分）
    void AddDefaultSearchPaths(const std::filesystem::path& exeDir,
                               const std::filesystem::path& projectDir = {});

    // 进程内（官方静态）插件注册。
    bool RegisterStatic(std::shared_ptr<IPluginInteraction> plugin);
    // 注册编译期注册表（EDA_REGISTER_PLUGIN）中的全部官方插件；返回注册数。
    std::size_t RegisterBuiltins();

    // 扫描发现 / 逐一校验与加载。返回本次新增/就绪数量。
    std::size_t Discover();
    std::size_t LoadAll();

    // 查询。
    std::vector<PluginRecord> Records() const;
    std::vector<PluginInfo> ProvidersOf(const std::string& capability) const;
    IPluginInteraction* Get(const std::string& id) const;
    // 按能力选择实例（preferredId 命中优先）；仅进程内插件有实例，动态插件返回 nullptr。
    IPluginInteraction* Select(const std::string& capability,
                               const std::string& preferredId = {}) const;
    std::string DiagnosticReport() const;

private:
    struct LoadedModule {
        std::unique_ptr<platform::DynamicLibrary> library;
        std::string id;
        void* instance = nullptr;
        std::function<void(void*)> unregister;
    };

    mutable std::mutex mutex_;
    std::vector<std::filesystem::path> searchPaths_;
    std::vector<std::string> pathDiagnostics_;
    std::vector<PluginRecord> records_;
    std::map<std::string, std::shared_ptr<IPluginInteraction>> instances_;
    std::vector<LoadedModule> modules_;
};

} // namespace eda
