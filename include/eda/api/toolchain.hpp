#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace eda {

struct ToolQuery {
    std::string name;                                // 主名（不含后缀），如 "yosys"
    std::vector<std::string> alternativeNames;       // 备用名，如 {"yosys.exe"}
    std::string configuredPath;                      // 项目配置的显式路径
    std::vector<std::filesystem::path> bundledRoots; // 随包运行时根，如 .../runtime
    std::vector<std::filesystem::path> bundledSubdirectories; // 根下相对子目录，如 apicula/Scripts
    std::vector<std::string> environmentVariables;   // 如 {"SIGFLOW_YOSYS"}
    bool searchPath = true;                          // 是否查 PATH
};

struct ToolResolution {
    bool found = false;
    std::filesystem::path path;
    std::string source;   // "configured" | "bundled" | "env" | "path"
    std::string reason;   // 未找到时的明确原因（修 R19：绝不返回误导性默认值）
};

// 工具发现链三合一：配置 → 随包 runtime → 环境变量 → PATH。
class IToolchain {
public:
    virtual ~IToolchain() = default;
    virtual ToolResolution Resolve(const ToolQuery& query) const = 0;
    virtual std::string ExecutableSuffix() const = 0;
    virtual char PathListSeparator() const = 0;
};

} // namespace eda
