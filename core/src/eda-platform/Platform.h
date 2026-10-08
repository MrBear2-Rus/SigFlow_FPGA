#pragma once

#include <filesystem>
#include <string>

namespace eda {
namespace platform {

// 平台差异集中处：业务/核心代码不得直接使用 #ifdef，一律经此接口。

std::string PathToUtf8(const std::filesystem::path& path);
// PathToUtf8 的逆运算：把 UTF-8 字符串还原为 path。
// Windows 上必须用它，而不能写 std::filesystem::path(utf8)——后者会把每个字节
// 当作一个宽字符，产生双重编码（中文路径会失效）。
std::filesystem::path PathFromUtf8(const std::string& utf8);

// 工程/快照相对路径的契约表示：UTF-8 编码 + POSIX 分隔符（'/'）。
// 不能写成 PathToUtf8(path.generic_string())：generic_string() 在 MinGW 下会先把
// UTF-16 窄化，非 ASCII 路径会被双重编码；也不能直接用 PathToUtf8(path)，
// 因为 Windows 的本地分隔符是 '\\'，而契约要求 POSIX 形式。
std::string RelativePathToUtf8(const std::filesystem::path& path);

// 按 UTF-8 读取环境变量（未设置返回空串）。
// Windows 上 std::getenv 走 ANSI 代码页，会把中文目录读成乱码字节；
// 这里统一走 W 系列 API + 码点级转码，保证 PATH 等含非 ASCII 的值往返无损。
std::string EnvUtf8(const std::string& name);

std::string UtcTimestamp();         // "YYYY-MM-DDTHH:MM:SSZ"
std::string UtcCompactTimestamp();  // "YYYYMMDDTHHMMSS"

// 应用数据根目录（跨平台）：
//   Windows: %LOCALAPPDATA%\SigFlow      POSIX: $XDG_DATA_HOME/sigflow 或 ~/.local/share/sigflow
// 目录不保证存在；调用方负责创建。两者都不可用时返回空路径。
std::filesystem::path AppDataRoot();
// 由规范化绝对路径生成稳定的工程 key（小写十六进制，无路径分隔符）；用于 Job/快照目录隔离。
std::string StablePathKey(const std::filesystem::path& canonicalPath);

const char* ExecutableSuffix();     // Windows ".exe"；POSIX ""
char PathListSeparator();           // Windows ';'；POSIX ':'
bool IsSharedLibraryExtension(const std::string& extension); // .dll/.so/.dylib（Windows 大小写不敏感）
std::string CompilerInfo();         // 构建指纹用："gcc-12.2.0" | "msvc-1930" | "unknown"

} // namespace platform
} // namespace eda
