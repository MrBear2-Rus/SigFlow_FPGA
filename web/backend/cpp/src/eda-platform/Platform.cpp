#include "Platform.h"

#include "Sha256.h"

#include <cctype>
#include <cstdlib>
#include <ctime>
#include <system_error>


namespace eda {
namespace platform {
namespace {

std::string FormatUtc(const char* format) {
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
    gmtime_r(&now, &tm);
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), format, &tm);
    return buffer;
}

} // namespace

std::string PathToUtf8(const std::filesystem::path& path) {
#if defined(__cpp_char8_t)
    const std::u8string value = path.u8string();
    return std::string(reinterpret_cast<const char*>(value.data()), value.size());
#else
    return path.u8string();
#endif
}

std::string EnvUtf8(const std::string& name) {
    const char* value = std::getenv(name.c_str());
    return value != nullptr ? std::string(value) : std::string();
}

std::filesystem::path PathFromUtf8(const std::string& utf8) {
    return std::filesystem::path(utf8);
}

std::string RelativePathToUtf8(const std::filesystem::path& path) {
    return PathToUtf8(path); // Linux already uses POSIX separators; backslash is a valid filename byte.
}

std::string UtcTimestamp() { return FormatUtc("%Y-%m-%dT%H:%M:%SZ"); }
std::string UtcCompactTimestamp() { return FormatUtc("%Y%m%dT%H%M%S"); }

const char* ExecutableSuffix() {
    return "";
}

char PathListSeparator() {
    return ':';
}

bool IsSharedLibraryExtension(const std::string& extension) {
    return extension == ".so";
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
    if (const char* xdg = std::getenv("XDG_DATA_HOME"); xdg != nullptr && *xdg != '\0') {
        return std::filesystem::path(xdg) / "sigflow";
    }
    if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
        return std::filesystem::path(home) / ".local" / "share" / "sigflow";
    }
    return {};
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
