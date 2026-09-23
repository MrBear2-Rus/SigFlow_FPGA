#include "PluginHost.h"

#include "DynamicLibrary.h"
#include "Platform.h"

#include <eda/api/abi.h>
#include <eda/api/build_info.h>
#include <eda/api/plugin_registry.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <set>
#include <sstream>

namespace eda {
namespace {

std::string PathToUtf8(const std::filesystem::path& path) {
    return platform::PathToUtf8(path);
}

bool IsSharedLibrary(const std::string& extension) {
    return platform::IsSharedLibraryExtension(extension);
}

std::vector<std::string> ParseCapabilities(const char* manifestJson) {
    std::vector<std::string> capabilities;
    if (manifestJson == nullptr) return capabilities;
    try {
        const Json manifest = Json::parse(manifestJson);
        if (manifest.contains("capabilities") && manifest["capabilities"].is_array()) {
            for (const auto& entry : manifest["capabilities"]) {
                if (entry.is_string()) capabilities.push_back(entry.get<std::string>());
            }
        }
    } catch (const std::exception&) {
        // manifest 非法不阻断加载，仅不提供能力声明。
    }
    return capabilities;
}

bool HasCapability(const PluginRecord& record, const std::string& capability) {
    return std::find(record.capabilities.begin(), record.capabilities.end(), capability) !=
           record.capabilities.end();
}

} // namespace

const char* ToString(PluginStatus status) {
    switch (status) {
        case PluginStatus::Discovered: return "Discovered";
        case PluginStatus::Validated:  return "Validated";
        case PluginStatus::Resolved:   return "Resolved";
        case PluginStatus::Loaded:     return "Loaded";
        case PluginStatus::Ready:      return "Ready";
        case PluginStatus::Error:      return "Error";
    }
    return "Unknown";
}

PluginHost::PluginHost() = default;

PluginHost::~PluginHost() {
    for (auto& module : modules_) {
        if (module.unregister && module.instance) {
            module.unregister(module.instance);
        }
        if (module.library) {
            module.library->Unload();
        }
    }
    modules_.clear();
}

void PluginHost::AddSearchPath(const std::filesystem::path& directory) {
    if (directory.empty()) return;
    std::lock_guard<std::mutex> lock(mutex_);
    searchPaths_.push_back(directory);
}

void PluginHost::AddSearchPathsFromEnvironment() {
    const char* raw = std::getenv("SIGFLOW_PLUGIN_PATH");
    if (raw == nullptr) return;
    const char separator = platform::PathListSeparator();
    const std::string value(raw);
    std::lock_guard<std::mutex> lock(mutex_);
    std::size_t start = 0;
    while (start <= value.size()) {
        const std::size_t position = value.find(separator, start);
        const std::size_t length =
            position == std::string::npos ? std::string::npos : position - start;
        const std::string item = value.substr(start, length);
        if (!item.empty()) searchPaths_.push_back(item);
        if (position == std::string::npos) break;
        start = position + 1;
    }
}

void PluginHost::AddDefaultSearchPaths(const std::filesystem::path& exeDir,
                                       const std::filesystem::path& projectDir) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!exeDir.empty()) {
        searchPaths_.push_back(exeDir / "plugins");
    }
    const char* home = std::getenv("USERPROFILE");
    if (home == nullptr) home = std::getenv("HOME");
    if (home != nullptr) {
        searchPaths_.push_back(std::filesystem::path(home) / ".sigflow" / "plugins");
    }
    if (!projectDir.empty()) {
        searchPaths_.push_back(projectDir / ".sigflow" / "plugins");
    }
    const char* raw = std::getenv("SIGFLOW_PLUGIN_PATH");
    if (raw != nullptr) {
        const char separator = platform::PathListSeparator();
        const std::string value(raw);
        std::size_t start = 0;
        while (start <= value.size()) {
            const std::size_t position = value.find(separator, start);
            const std::size_t length =
                position == std::string::npos ? std::string::npos : position - start;
            const std::string item = value.substr(start, length);
            if (!item.empty()) searchPaths_.push_back(item);
            if (position == std::string::npos) break;
            start = position + 1;
        }
    }
}

bool PluginHost::RegisterStatic(std::shared_ptr<IPluginInteraction> plugin) {
    if (!plugin) return false;
    const PluginInfo info = plugin->info();
    if (info.id.empty()) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    if (instances_.find(info.id) != instances_.end()) return false;
    PluginRecord record;
    record.id = info.id;
    record.version = info.version;
    record.status = PluginStatus::Ready;
    record.location = info.location.empty() ? "inner" : info.location;
    record.runtime = info.runtime.empty() ? "inprocess" : info.runtime;
    record.capabilities = info.capabilities;
    record.isStatic = true;
    records_.push_back(record);
    instances_[info.id] = std::move(plugin);
    return true;
}

std::size_t PluginHost::RegisterBuiltins() {
    std::size_t registered = 0;
    for (const auto& entry : StaticPluginRegistry()) {
        if (entry.factory && RegisterStatic(entry.factory())) {
            ++registered;
        }
    }
    return registered;
}

std::size_t PluginHost::Discover() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::size_t added = 0;
    const auto known = [this](const std::filesystem::path& path) {
        for (const auto& record : records_) {
            if (!record.path.empty() && record.path == path) return true;
        }
        return false;
    };
    for (const auto& directory : searchPaths_) {
        std::error_code existsError;
        if (!std::filesystem::exists(directory, existsError)) {
            pathDiagnostics_.push_back("plugin directory not found: " + directory.string());
            continue;
        }
        std::error_code iterateError;
        for (std::filesystem::directory_iterator it(directory, iterateError), end; it != end;
             it.increment(iterateError)) {
            if (iterateError) {
                pathDiagnostics_.push_back("plugin enumeration failed in " + directory.string() +
                                           ": " + iterateError.message());
                break;
            }
            std::error_code typeError;
            if (!it->is_regular_file(typeError)) continue;
            if (!IsSharedLibrary(it->path().extension().string())) continue;
            if (known(it->path())) continue;
            PluginRecord record;
            record.path = it->path();
            record.status = PluginStatus::Discovered;
            record.location = "external";
            record.runtime = "inprocess";
            records_.push_back(record);
            ++added;
        }
    }
    return added;
}

std::size_t PluginHost::LoadAll() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::size_t ready = 0;
    const std::string hostBuild = BuildInfo();

    for (auto& record : records_) {
        if (record.isStatic || record.status != PluginStatus::Discovered) continue;

        auto library = std::make_unique<platform::DynamicLibrary>();
        if (!library->Load(PathToUtf8(record.path))) {
            record.status = PluginStatus::Error;
            record.error = "load failed: " + library->LastError();
            continue;
        }
        record.status = PluginStatus::Loaded;

        using QueryFn = const eda_plugin_descriptor_v1* (*)(std::uint32_t);
        const auto query = reinterpret_cast<QueryFn>(library->Symbol("eda_plugin_query_v1"));
        if (query == nullptr) {
            record.status = PluginStatus::Error;
            record.error = "missing export: eda_plugin_query_v1";
            continue;
        }

        const eda_plugin_descriptor_v1* descriptor = query(EDA_PLUGIN_ABI_VERSION);
        if (descriptor == nullptr) {
            record.status = PluginStatus::Error;
            record.error = "ABI handshake rejected (host ABI " +
                           std::to_string(EDA_PLUGIN_ABI_VERSION) + ")";
            continue;
        }
        // 尽早在 descriptor 可用时回填 id/version：即使后续校验失败，错误记录也可被识别（R1）。
        record.id = descriptor->id != nullptr ? descriptor->id : "";
        record.version = descriptor->version != nullptr ? descriptor->version : "";
        if (descriptor->struct_size < sizeof(eda_plugin_descriptor_v1)) {
            record.status = PluginStatus::Error;
            record.error = "descriptor too small (struct_size=" +
                           std::to_string(descriptor->struct_size) + ")";
            continue;
        }
        if (descriptor->abi_version != EDA_PLUGIN_ABI_VERSION) {
            record.status = PluginStatus::Error;
            record.error = "descriptor ABI mismatch (plugin=" +
                           std::to_string(descriptor->abi_version) + ", host=" +
                           std::to_string(EDA_PLUGIN_ABI_VERSION) + ")";
            continue;
        }
        const std::string pluginBuild = descriptor->build_info ? descriptor->build_info : "";
        if (pluginBuild != hostBuild) {
            record.status = PluginStatus::Error;
            record.error = "build fingerprint mismatch (plugin='" + pluginBuild + "', host='" +
                           hostBuild + "')";
            continue;
        }
        if (descriptor->id == nullptr) {
            record.status = PluginStatus::Error;
            record.error = "descriptor missing id";
            continue;
        }

        record.capabilities = ParseCapabilities(descriptor->manifest_json);
        record.status = PluginStatus::Validated;

        void* instance = nullptr;
        if (descriptor->register_plugin != nullptr) {
            descriptor->register_plugin(nullptr, &instance);
        }
        record.status = PluginStatus::Ready;

        LoadedModule module;
        module.library = std::move(library);
        module.id = record.id;
        module.instance = instance;
        if (descriptor->unregister_plugin != nullptr) {
            module.unregister = [function = descriptor->unregister_plugin](void* handle) {
                function(handle);
            };
        }
        modules_.push_back(std::move(module));
        ++ready;
    }
    return ready;
}

std::vector<PluginRecord> PluginHost::Records() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return records_;
}

std::vector<PluginInfo> PluginHost::ProvidersOf(const std::string& capability) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<PluginInfo> result;
    for (const auto& record : records_) {
        if (record.status != PluginStatus::Ready || !HasCapability(record, capability)) continue;
        PluginInfo info;
        info.id = record.id;
        info.version = record.version;
        info.location = record.location;
        info.runtime = record.runtime;
        info.capabilities = record.capabilities;
        result.push_back(std::move(info));
    }
    return result;
}

IPluginInteraction* PluginHost::Get(const std::string& id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = instances_.find(id);
    return it == instances_.end() ? nullptr : it->second.get();
}

IPluginInteraction* PluginHost::Select(const std::string& capability,
                                       const std::string& preferredId) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!preferredId.empty()) {
        const auto instanceIt = instances_.find(preferredId);
        if (instanceIt != instances_.end()) {
            for (const auto& record : records_) {
                if (record.id == preferredId && record.status == PluginStatus::Ready &&
                    HasCapability(record, capability)) {
                    return instanceIt->second.get();
                }
            }
        }
    }
    for (const auto& record : records_) {
        if (record.status != PluginStatus::Ready || !HasCapability(record, capability)) continue;
        const auto instanceIt = instances_.find(record.id);
        if (instanceIt != instances_.end()) {
            return instanceIt->second.get();
        }
    }
    return nullptr;
}

std::string PluginHost::DiagnosticReport() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::ostringstream stream;
    stream << "plugin host diagnostics:\n";
    for (const auto& diagnostic : pathDiagnostics_) {
        stream << "  path: " << diagnostic << "\n";
    }
    for (const auto& record : records_) {
        stream << "  [" << ToString(record.status) << "] "
               << (record.id.empty() ? "(unidentified)" : record.id);
        if (!record.path.empty()) stream << " <- " << record.path.string();
        if (!record.error.empty()) stream << " ERROR: " << record.error;
        stream << "\n";
    }
    return stream.str();
}

} // namespace eda
