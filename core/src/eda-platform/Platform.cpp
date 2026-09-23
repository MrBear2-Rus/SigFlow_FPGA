#include "Platform.h"

#include <cctype>
#include <ctime>

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
#if defined(__cpp_char8_t)
    const std::u8string value = path.u8string();
    return std::string(reinterpret_cast<const char*>(value.data()), value.size());
#else
    return path.u8string();
#endif
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

} // namespace platform
} // namespace eda
