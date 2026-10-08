#include "Platform.h"

#include "Sha256.h"

#include <cctype>
#include <cstdlib>
#include <ctime>
#include <system_error>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace eda {
namespace platform {
namespace {

std::string FormatUtc(const char* format) {
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &now);
#else
    gmtime_r(&now, &tm);
#endif
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), format, &tm);
    return buffer;
}

} // namespace

std::string PathToUtf8(const std::filesystem::path& path) {
#if defined(_WIN32)
    // Windows：path::native() 是 UTF-16。必须按码点转 UTF-8，不能依赖
    // path.u8string()——在 MinGW/libstdc++ 下它会对非 ASCII 产生双重编码
    // （例如 "中文" 变成 C3A4 C2B8 ...），导致中文路径写进 Yosys 脚本后找不到文件。
    const std::wstring native = path.native();
    std::string utf8;
    utf8.reserve(native.size() * 3);
    const auto appendUtf8 = [&utf8](unsigned int codePoint) {
        if (codePoint < 0x80) {
            utf8.push_back(static_cast<char>(codePoint));
        } else if (codePoint < 0x800) {
            utf8.push_back(static_cast<char>(0xC0 | (codePoint >> 6)));
            utf8.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
        } else if (codePoint < 0x10000) {
            utf8.push_back(static_cast<char>(0xE0 | (codePoint >> 12)));
            utf8.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
            utf8.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
        } else {
            utf8.push_back(static_cast<char>(0xF0 | (codePoint >> 18)));
            utf8.push_back(static_cast<char>(0x80 | ((codePoint >> 12) & 0x3F)));
            utf8.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
            utf8.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
        }
    };
    for (std::size_t index = 0; index < native.size(); ++index) {
        unsigned int codePoint = static_cast<unsigned int>(native[index]);
        // Windows wchar_t 是 UTF-16。不能把代理项当作独立码点编码，否则非 BMP
        // 字符会生成无效 UTF-8，进而破坏 PathToUtf8/PathFromUtf8 的往返性质。
        if (codePoint >= 0xD800 && codePoint <= 0xDBFF && index + 1 < native.size()) {
            const unsigned int low = static_cast<unsigned int>(native[index + 1]);
            if (low >= 0xDC00 && low <= 0xDFFF) {
                codePoint = 0x10000 + ((codePoint - 0xD800) << 10) + (low - 0xDC00);
                ++index;
            } else {
                codePoint = 0xFFFD;
            }
        } else if (codePoint >= 0xD800 && codePoint <= 0xDFFF) {
            codePoint = 0xFFFD;
        }
        appendUtf8(codePoint);
    }
    return utf8;
#elif defined(__cpp_char8_t)
    const std::u8string value = path.u8string();
    return std::string(reinterpret_cast<const char*>(value.data()), value.size());
#else
    return path.u8string();
#endif
}

std::string EnvUtf8(const std::string& name) {
#if defined(_WIN32)
    const std::wstring wideName(name.begin(), name.end());
    const DWORD size = ::GetEnvironmentVariableW(wideName.c_str(), nullptr, 0);
    if (size == 0) return std::string();
    std::wstring value(static_cast<std::size_t>(size), L'\0');
    const DWORD written =
        ::GetEnvironmentVariableW(wideName.c_str(), value.data(), size);
    if (written == 0) return std::string();
    value.resize(written);
    // 复用 PathToUtf8 的码点级 UTF-16→UTF-8 编码（std::getenv 走 ANSI 代码页，
    // 中文环境变量值会乱码；构造 fs::path 以复用既有编码器，不复制实现）。
    return PathToUtf8(std::filesystem::path(value));
#else
    const char* value = std::getenv(name.c_str());
    return value != nullptr ? std::string(value) : std::string();
#endif
}

std::filesystem::path PathFromUtf8(const std::string& utf8) {
#if defined(_WIN32)
    // 与 PathToUtf8 互逆：按 UTF-8 解码成 UTF-16 再构造 path。
    // 直接用 std::filesystem::path(utf8) 会把每个字节当成一个宽字符（见上）。
    std::wstring wide;
    wide.reserve(utf8.size());
    std::size_t index = 0;
    const auto appendWide = [&wide](unsigned int codePoint) {
        if (codePoint <= 0xFFFF) {
            wide.push_back(static_cast<wchar_t>(codePoint));
            return;
        }
        codePoint -= 0x10000;
        wide.push_back(static_cast<wchar_t>(0xD800 + (codePoint >> 10)));
        wide.push_back(static_cast<wchar_t>(0xDC00 + (codePoint & 0x3FF)));
    };
    while (index < utf8.size()) {
        const unsigned char lead = static_cast<unsigned char>(utf8[index]);
        unsigned int codePoint = 0;
        std::size_t extra = 0;
        unsigned int minimum = 0;
        if (lead < 0x80) {
            codePoint = lead;
        } else if ((lead & 0xE0) == 0xC0) {
            codePoint = lead & 0x1F;
            extra = 1;
            minimum = 0x80;
        } else if ((lead & 0xF0) == 0xE0) {
            codePoint = lead & 0x0F;
            extra = 2;
            minimum = 0x800;
        } else if ((lead & 0xF8) == 0xF0) {
            codePoint = lead & 0x07;
            extra = 3;
            minimum = 0x10000;
        } else {
            // 非法 UTF-8 不能悄悄按 Latin-1 改写为另一条路径；保留一个可见的
            // replacement character，且继续解析后续输入。
            appendWide(0xFFFD);
            ++index;
            continue;
        }
        if (index + extra >= utf8.size()) {
            appendWide(0xFFFD);
            ++index;
            continue;
        }
        bool valid = true;
        for (std::size_t offset = 1; offset <= extra; ++offset) {
            const unsigned char continuation = static_cast<unsigned char>(utf8[index + offset]);
            if ((continuation & 0xC0) != 0x80) {
                valid = false;
                break;
            }
            codePoint = (codePoint << 6) | (continuation & 0x3F);
        }
        if (!valid) {
            appendWide(0xFFFD);
            ++index;
            continue;
        }
        index += extra + 1;
        if (codePoint < minimum || codePoint > 0x10FFFF ||
            (codePoint >= 0xD800 && codePoint <= 0xDFFF)) {
            appendWide(0xFFFD);
            continue;
        }
        appendWide(codePoint);
    }
    return std::filesystem::path(wide);
#else
    return std::filesystem::path(utf8);
#endif
}

std::string RelativePathToUtf8(const std::filesystem::path& path) {
    std::string value = PathToUtf8(path);
    // Windows 的 path 用 '\\'；契约要求工程相对 POSIX 形式。
    for (char& character : value) {
        if (character == '\\') character = '/';
    }
    return value;
}

std::string UtcTimestamp() { return FormatUtc("%Y-%m-%dT%H:%M:%SZ"); }
std::string UtcCompactTimestamp() { return FormatUtc("%Y%m%dT%H%M%S"); }

const char* ExecutableSuffix() {
#if defined(_WIN32)
    return ".exe";
#else
    return "";
#endif
}

char PathListSeparator() {
#if defined(_WIN32)
    return ';';
#else
    return ':';
#endif
}

bool IsSharedLibraryExtension(const std::string& extension) {
#if defined(_WIN32)
    std::string lowered = extension;
    for (char& c : lowered) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return lowered == ".dll";
#else
    return extension == ".so" || extension == ".dylib";
#endif
}

std::string CompilerInfo() {
#if defined(__GNUC__)
    return "gcc-" + std::to_string(__GNUC__) + "." + std::to_string(__GNUC_MINOR__) + "." +
           std::to_string(__GNUC_PATCHLEVEL__);
#elif defined(_MSC_VER)
    return "msvc-" + std::to_string(_MSC_VER);
#else
    return "unknown";
#endif
}

std::filesystem::path AppDataRoot() {
#if defined(_WIN32)
    // 优先 LOCALAPPDATA（不随域漫游同步），回退 APPDATA。
    for (const char* name : {"LOCALAPPDATA", "APPDATA"}) {
        const char* value = std::getenv(name);
        if (value == nullptr || *value == '\0') continue;
        return std::filesystem::path(value) / "SigFlow";
    }
    return {};
#else
    if (const char* xdg = std::getenv("XDG_DATA_HOME"); xdg != nullptr && *xdg != '\0') {
        return std::filesystem::path(xdg) / "sigflow";
    }
    if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
        return std::filesystem::path(home) / ".local" / "share" / "sigflow";
    }
    return {};
#endif
}

std::string StablePathKey(const std::filesystem::path& canonicalPath) {
    std::error_code ec;
    std::filesystem::path canonical = std::filesystem::weakly_canonical(canonicalPath, ec);
    if (ec || canonical.empty()) canonical = canonicalPath;
    const std::string material = PathToUtf8(canonical.lexically_normal());
    const std::string hex = Sha256Hex(material.data(), material.size());
    return hex.substr(0, 24);
}

} // namespace platform
} // namespace eda
