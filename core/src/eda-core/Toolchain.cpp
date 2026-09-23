#include "Toolchain.h"

#include "Platform.h"

#include <cstdlib>
#include <utility>

namespace eda {
namespace {

std::string DefaultEnvironment(const std::string& name) {
    const char* value = std::getenv(name.c_str());
    return value != nullptr ? value : "";
}

bool FileExists(const std::filesystem::path& path) {
    std::error_code error;
    return std::filesystem::is_regular_file(path, error);
}

std::string WithSuffix(const std::string& name, const std::string& suffix) {
    if (suffix.empty()) return name;
    if (name.size() >= suffix.size() &&
        name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0) {
        return name;
    }
    return name + suffix;
}

} // namespace

DefaultToolchain::DefaultToolchain(std::vector<std::filesystem::path> bundledRoots,
                                   EnvironmentLookup environmentLookup)
    : bundledRoots_(std::move(bundledRoots)), environmentLookup_(std::move(environmentLookup)) {
    if (!environmentLookup_) {
        environmentLookup_ = DefaultEnvironment;
    }
}

std::string DefaultToolchain::ExecutableSuffix() const { return platform::ExecutableSuffix(); }

char DefaultToolchain::PathListSeparator() const { return platform::PathListSeparator(); }

ToolResolution DefaultToolchain::Resolve(const ToolQuery& query) const {
    ToolResolution result;
    std::vector<std::string> names;
    if (!query.name.empty()) names.push_back(query.name);
    for (const auto& name : query.alternativeNames) {
        if (!name.empty()) names.push_back(name);
    }
    if (names.empty()) {
        result.reason = "tool query has an empty name";
        return result;
    }

    const std::string suffix = ExecutableSuffix();

    // 1) 项目配置的显式路径。
    if (!query.configuredPath.empty()) {
        const std::filesystem::path configured = query.configuredPath;
        if (FileExists(configured)) {
            result.found = true;
            result.path = configured;
            result.source = "configured";
            return result;
        }
        result.reason += "configured path not found: " + query.configuredPath + "; ";
    }

    // 2) 随包运行时（配置根 + 构造时提供的根）。
    std::vector<std::filesystem::path> roots = query.bundledRoots;
    roots.insert(roots.end(), bundledRoots_.begin(), bundledRoots_.end());
    for (const auto& root : roots) {
        for (const auto& name : names) {
            const std::string exeName = WithSuffix(name, suffix);
            std::vector<std::filesystem::path> candidates = {
                root / exeName,
                root / "bin" / exeName,
            };
            for (const auto& subdirectory : query.bundledSubdirectories) {
                candidates.push_back(root / subdirectory / exeName);
            }
            candidates.push_back(root / name / exeName);
            candidates.push_back(root / name / "bin" / exeName);
            for (const auto& candidate : candidates) {
                if (FileExists(candidate)) {
                    result.found = true;
                    result.path = candidate;
                    result.source = "bundled";
                    return result;
                }
            }
        }
    }
    if (!roots.empty()) result.reason += "not found under bundled roots; ";

    // 3) 环境变量。兼容"环境变量给的是裸工具名（无路径分隔符）"的旧行为。
    for (const auto& variable : query.environmentVariables) {
        const std::string value = environmentLookup_(variable);
        if (value.empty()) continue;
        if (FileExists(value)) {
            result.found = true;
            result.path = value;
            result.source = "env";
            return result;
        }
        if (value.find('/') == std::string::npos && value.find('\\') == std::string::npos &&
            FileExists(WithSuffix(value, suffix))) {
            result.found = true;
            result.path = WithSuffix(value, suffix);
            result.source = "env";
            return result;
        }
    }
    if (!query.environmentVariables.empty()) {
        result.reason += "not found via environment variables; ";
    }

    // 4) PATH。去除条目首尾空白与成对引号（与旧实现一致）。
    if (query.searchPath) {
        const std::string pathValue = environmentLookup_("PATH");
        std::size_t start = 0;
        while (start <= pathValue.size()) {
            const std::size_t position = pathValue.find(PathListSeparator(), start);
            const std::size_t length =
                position == std::string::npos ? std::string::npos : position - start;
            std::string directory = pathValue.substr(start, length);
            const std::size_t first = directory.find_first_not_of(" \t");
            const std::size_t last = directory.find_last_not_of(" \t");
            directory = first == std::string::npos ? std::string()
                                                   : directory.substr(first, last - first + 1);
            if (directory.size() >= 2 && directory.front() == '"' && directory.back() == '"') {
                directory = directory.substr(1, directory.size() - 2);
            }
            if (!directory.empty()) {
                for (const auto& name : names) {
                    const std::filesystem::path candidate =
                        std::filesystem::path(directory) / WithSuffix(name, suffix);
                    if (FileExists(candidate)) {
                        result.found = true;
                        result.path = candidate;
                        result.source = "path";
                        return result;
                    }
                }
            }
            if (position == std::string::npos) break;
            start = position + 1;
        }
        result.reason += "not found on PATH; ";
    }

    result.reason = "tool not found: " + query.name + " (" + result.reason + ")";
    return result;
}

} // namespace eda
