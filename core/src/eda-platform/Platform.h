#pragma once

#include <filesystem>
#include <string>

namespace eda {
namespace platform {

// 平台差异集中处：业务/核心代码不得直接使用 #ifdef，一律经此接口。

std::string PathToUtf8(const std::filesystem::path& path);

std::string UtcTimestamp();         // "YYYY-MM-DDTHH:MM:SSZ"
std::string UtcCompactTimestamp();  // "YYYYMMDDTHHMMSS"

const char* ExecutableSuffix();     // Windows ".exe"；POSIX ""
char PathListSeparator();           // Windows ';'；POSIX ':'
bool IsSharedLibraryExtension(const std::string& extension); // .dll/.so/.dylib（Windows 大小写不敏感）
std::string CompilerInfo();         // 构建指纹用："gcc-12.2.0" | "msvc-1930" | "unknown"

} // namespace platform
} // namespace eda
