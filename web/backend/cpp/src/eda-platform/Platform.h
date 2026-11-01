#pragma once
#include <filesystem>
#include <string>
namespace eda::platform {
// Linux paths and environment use UTF-8. No wxWidgets dependency.
std::string PathToUtf8(const std::filesystem::path& path);
std::filesystem::path PathFromUtf8(const std::string& utf8);
std::string RelativePathToUtf8(const std::filesystem::path& path);
std::string EnvUtf8(const std::string& name);
std::string UtcTimestamp();
std::string UtcCompactTimestamp();
std::filesystem::path AppDataRoot();
std::string StablePathKey(const std::filesystem::path& canonicalPath);
const char* ExecutableSuffix();
char PathListSeparator();
bool IsSharedLibraryExtension(const std::string& extension);
std::string CompilerInfo();
}  // namespace eda::platform
