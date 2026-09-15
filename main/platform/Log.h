#pragma once

// 统一日志入口：业务代码只依赖这里，不直接触碰 OS 调试 API。
// Windows: OutputDebugStringA；其他平台: stderr。
#include <wx/string.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
inline void SigFlowLogImpl(const wxString& text)
{
    const wxScopedCharBuffer utf8 = text.ToUTF8();
    if (utf8.data()) OutputDebugStringA(utf8.data());
}
#else
#include <cstdio>
inline void SigFlowLogImpl(const wxString& text)
{
    const wxScopedCharBuffer utf8 = text.ToUTF8();
    if (utf8.data()) std::fprintf(stderr, "%s", utf8.data());
}
#endif

inline void SigFlowLogImpl(const char* utf8Text)
{
    SigFlowLogImpl(wxString::FromUTF8(utf8Text));
}

#define SIGFLOW_LOG(message) SigFlowLogImpl(message)
