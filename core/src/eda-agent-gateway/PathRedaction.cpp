#include "PathRedaction.h"

#include <array>
#include <cctype>

namespace eda {
namespace agent {
namespace {

constexpr std::array<const char*, 12> kPosixRoots{
    "/usr/", "/opt/", "/home/", "/tmp/",      "/var/", "/etc/",
    "/mnt/", "/media/", "/root/", "/dev/",    "/proc/", "/sys/",
};

bool StartsWithPosixRoot(const std::string& text) {
    for (const char* root : kPosixRoots) {
        if (text.rfind(root, 0) == 0) return true;
    }
    return false;
}

bool IsPathSeparator(char character) { return character == '\\' || character == '/'; }

// 路径 token 的结束位置：空白、引号、以及常见的日志/JSON 分隔符。
// 注意：盘符后的 ':'（`C:\`）不是分隔符，否则 Windows 路径会被截成单字母。
std::size_t FindTokenEnd(const std::string& text, std::size_t start) {
    std::size_t end = start;
    while (end < text.size()) {
        const char character = text[end];
        const bool driveColon = character == ':' && end == start + 1 &&
                                std::isalpha(static_cast<unsigned char>(text[start]));
        if (!driveColon &&
            (character == ' ' || character == '\t' || character == '\n' ||
             character == '\r' || character == '"' || character == '\'' ||
             character == '`' || character == ',' || character == ';' || character == ')' ||
             character == '(' || character == ']' || character == '[' || character == '}' ||
             character == '{' || character == '<' || character == '>' || character == '|' ||
             character == ':' || character == '*')) {
            break;
        }
        ++end;
    }
    return end;
}

// 以 [start,end) 为候选 token，判断是否为可裁剪的本机绝对路径。
bool IsRedactableAt(const std::string& text, std::size_t start, std::size_t end) {
    if (end <= start) return false;
    // Windows 盘符：X:\ 或 X:/
    if (end - start >= 3 && std::isalpha(static_cast<unsigned char>(text[start])) &&
        text[start + 1] == ':' && IsPathSeparator(text[start + 2])) {
        return true;
    }
    // UNC：\\server\share
    if (end - start >= 4 && text[start] == '\\' && text[start + 1] == '\\') return true;
    // POSIX：以常见根前缀开头，且后面还有至少一段
    return StartsWithPosixRoot(text.substr(start, end - start));
}

} // namespace

bool LooksLikeAbsolutePath(const std::string& text) {
    if (text.empty()) return false;
    if (text.size() >= 3 && std::isalpha(static_cast<unsigned char>(text[0])) && text[1] == ':' &&
        IsPathSeparator(text[2])) {
        return true;  // Windows 盘符路径
    }
    if (text.rfind("\\\\", 0) == 0) return true;  // UNC
    // POSIX：绝对路径且不是 URL/接口路径（至少两级且首段是已知根）。
    if (!text.empty() && text[0] == '/' && text.find('/', 1) != std::string::npos) {
        return StartsWithPosixRoot(text) || text.rfind("//", 0) == 0;
    }
    return false;
}

std::string BasenameOfPath(const std::string& text) {
    const std::size_t separator = text.find_last_of("\\/");
    if (separator == std::string::npos) return text;
    const std::string tail = text.substr(separator + 1);
    return tail.empty() ? text : tail;
}

std::size_t RedactLocalPathsInText(std::string& text) {
    std::size_t replacements = 0;
    std::size_t index = 0;
    std::string result;
    result.reserve(text.size());
    while (index < text.size()) {
        const char character = text[index];
        const bool candidate = (character == '/' || character == '\\' ||
                                (std::isalpha(static_cast<unsigned char>(character)) &&
                                 index + 2 < text.size() && text[index + 1] == ':' &&
                                 IsPathSeparator(text[index + 2])));
        if (!candidate) {
            result.push_back(character);
            ++index;
            continue;
        }
        const std::size_t end = FindTokenEnd(text, index);
        if (IsRedactableAt(text, index, end)) {
            result += BasenameOfPath(text.substr(index, end - index));
            ++replacements;
            index = end;
            continue;
        }
        result.push_back(character);
        ++index;
    }
    if (replacements > 0) text = std::move(result);
    return replacements;
}

bool RedactAbsolutePathsInJson(Json& value) {
    bool redacted = false;
    if (value.is_string()) {
        const std::string original = value.get<std::string>();
        if (LooksLikeAbsolutePath(original)) {
            value = BasenameOfPath(original);
            return true;
        }
        std::string text = original;
        if (RedactLocalPathsInText(text) > 0) {
            value = text;
            redacted = true;
        }
        return redacted;
    }
    if (value.is_array()) {
        for (Json& item : value) redacted = RedactAbsolutePathsInJson(item) || redacted;
        return redacted;
    }
    if (value.is_object()) {
        for (auto it = value.begin(); it != value.end(); ++it) {
            redacted = RedactAbsolutePathsInJson(it.value()) || redacted;
        }
    }
    return redacted;
}

} // namespace agent
} // namespace eda
