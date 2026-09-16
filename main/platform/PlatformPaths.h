#pragma once

// 路径/环境/可执行文件后缀的平台差异集中处。业务代码通过这里取值。
#include <wx/string.h>
#include <wx/filename.h>
#include <wx/stdpaths.h>
#include <filesystem>
#include <cctype>
#include <string>
#include <vector>

namespace sigflow::platform {

inline wxUniChar PathSeparator()
{
#if defined(_WIN32)
    return wxUniChar('\\');
#else
    return wxUniChar('/');
#endif
}

// 以平台分隔符拼接两个路径片段（不做规范化，保持与原有字面拼接一致的语义）。
inline wxString JoinPath(const wxString& base, const wxString& leaf)
{
    return base + wxString(PathSeparator()) + leaf;
}

// std::string 版本（用于已使用 std::string 表示路径的代码）。
inline std::string JoinPath(const std::string& base, const std::string& leaf)
{
#if defined(_WIN32)
    const char separator = '\\';
#else
    const char separator = '/';
#endif
    return base + separator + leaf;
}

inline const char* ExecutableSuffix()
{
#if defined(_WIN32)
    return ".exe";
#else
    return "";
#endif
}

inline const char* SharedLibrarySuffix()
{
#if defined(_WIN32)
    return ".dll";
#else
    return ".so";
#endif
}

// PATH 变量的分隔符：Windows ';'，POSIX ':'。
inline wxUniChar PathListSeparator()
{
#if defined(_WIN32)
    return wxUniChar(';');
#else
    return wxUniChar(':');
#endif
}

// 把 PATH 变量切成目录列表（使用平台分隔符）。
inline std::vector<wxString> SplitPathVariable(const wxString& value)
{
    std::vector<wxString> parts;
    wxString current;
    const wxUniChar separator = PathListSeparator();
    for (wxString::const_iterator it = value.begin(); it != value.end(); ++it) {
        if (*it == separator) { parts.push_back(current); current.clear(); }
        else { current += *it; }
    }
    parts.push_back(current);
    return parts;
}

// 给工具基名补上平台可执行后缀（已带后缀则原样返回）。
inline wxString WithExecutableSuffix(const wxString& baseName)
{
    if (baseName.EndsWith(ExecutableSuffix())) return baseName;
    return baseName + ExecutableSuffix();
}

// ---- 可执行文件目录（定位随 exe 分发的数据文件，如 canvas_elements.json）----
// 不要用 wxGetCwd() 定位这类文件：工作目录取决于用户从哪儿启动程序，
// Windows 上 VS 调试恰好把 CWD 设为输出目录，Linux 下从别的目录启动就会找不到文件。
inline const wxString& ExecutableDir()
{
    static const wxString dir = [] {
        wxFileName exePath(wxStandardPaths::Get().GetExecutablePath());
        exePath.SetFullName(wxEmptyString);
        return exePath.GetPath();
    }();
    return dir;
}

// ---- 资源路径（统一管理，避免业务代码里散落 "res\\..." 相对路径）----
// 资源根目录：<可执行文件目录>/res（Windows/Linux 一致；构建后 res/ 会被拷到 exe 旁）。
inline const wxString& ResourceRoot()
{
    static const wxString root = ExecutableDir() + "/res";
    return root;
}

// 资源文件绝对路径：relative 可写 "res/icons/a.svg" 或 "icons/a.svg"；
// 内部统一把 '\' 规范化为 '/',因此 Linux 上也能正确解析。
inline wxString ResourcePath(const wxString& relative)
{
    wxString cleaned = relative;
    cleaned.Replace("\\", "/");
    if (cleaned == "res") cleaned.clear();
    else if (cleaned.StartsWith("res/")) cleaned = cleaned.Mid(4);
    return cleaned.IsEmpty() ? ResourceRoot() : ResourceRoot() + "/" + cleaned;
}

// UTF-8 路径 -> std::filesystem::path。
// C++20 起 std::filesystem::u8path 被弃用（GCC16 会告警/未来可能移除），这里统一封装。
inline std::filesystem::path Utf8Path(const char* utf8)
{
#if defined(__cpp_char8_t)
    return std::filesystem::path(reinterpret_cast<const char8_t*>(utf8));
#else
    return std::filesystem::u8path(utf8);
#endif
}

inline std::filesystem::path Utf8Path(const std::string& utf8)
{
    return Utf8Path(utf8.c_str());
}

inline std::filesystem::path Utf8Path(const wxString& utf8Text)
{
    const wxScopedCharBuffer utf8 = utf8Text.ToUTF8();
    return Utf8Path(utf8.data() != nullptr ? utf8.data() : "");
}

// std::filesystem::path -> UTF-8 std::string。
// 与 Utf8Path 配对使用：凡是"要交给非 wx API"的路径都必须走这一对，
// 不能用 path::string()（Windows 上是 ANSI 代码页，非 ASCII 会乱码/失败）。
inline std::string PathToUtf8(const std::filesystem::path& path)
{
#if defined(__cpp_char8_t)
    const std::u8string utf8 = path.u8string();
    return std::string(reinterpret_cast<const char*>(utf8.data()), utf8.size());
#else
    return path.u8string();
#endif
}

// 扩展名比较：Windows 文件系统大小写不敏感，".DLL" 也要能匹配 ".dll"。
// POSIX 上保持大小写敏感。
inline bool ExtensionEquals(const std::string& lhs, const std::string& rhs)
{
#if defined(_WIN32)
    if (lhs.size() != rhs.size()) return false;
    for (std::size_t i = 0; i < lhs.size(); ++i) {
        const unsigned char a = static_cast<unsigned char>(lhs[i]);
        const unsigned char b = static_cast<unsigned char>(rhs[i]);
        if (std::tolower(a) != std::tolower(b)) return false;
    }
    return true;
#else
    return lhs == rhs;
#endif
}

// wxString -> UTF-8 std::string。
//
// 用于所有"要交给非 wx API"的边界：tree-sitter、nlohmann、std::filesystem、
// 子进程参数、日志等。
//
// **不要用 wxString::ToStdString()**：它走的是当前 locale——
//   * Windows 上是 ANSI/CP936，中文会变成非法 UTF-8（nlohmann::dump 会抛
//     type_error.316，tree-sitter 会按错误字节切分）；
//   * Linux 在非 UTF-8 locale（如 LANG=C）下转换失败会**返回空串**，静默丢数据。
inline std::string Utf8String(const wxString& text)
{
    const wxScopedCharBuffer utf8 = text.ToUTF8();
    return std::string(utf8.data() != nullptr ? utf8.data() : "", utf8.length());
}

} // namespace sigflow::platform
