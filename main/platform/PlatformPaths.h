#pragma once

// 路径/环境/可执行文件后缀的平台差异集中处。业务代码通过这里取值。
#include <wx/string.h>
#include <wx/filename.h>
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

} // namespace sigflow::platform
