#include "DynamicLibrary.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace sigflow::platform {

DynamicLibrary::~DynamicLibrary() { Unload(); }

bool DynamicLibrary::Load(const std::string& path)
{
    Unload();
#if defined(_WIN32)
    // path 约定为 **UTF-8**（调用方用 platform::PathToUtf8 生成）。
    // 旧实现用 LoadLibraryA + 窄字符路径：Windows 上按 ANSI 代码页解释，
    // 形如 C:\Users\<中文名>\...\plugins 的路径会加载失败；
    // 而且 GetLastError() 被丢弃，失败原因完全不可见。
    const int wideLength = ::MultiByteToWideChar(
        CP_UTF8, 0, path.c_str(), static_cast<int>(path.size()), nullptr, 0);
    if (wideLength <= 0) {
        m_error = "plugin path is not valid UTF-8";
        return false;
    }
    std::wstring widePath(static_cast<std::size_t>(wideLength), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, path.c_str(), static_cast<int>(path.size()),
                          widePath.data(), wideLength);
    const HMODULE module = ::LoadLibraryW(widePath.c_str());
    if (module == nullptr) {
        m_error = "LoadLibraryW failed (Win32 error " +
                  std::to_string(::GetLastError()) + ")";
        return false;
    }
    m_handle = reinterpret_cast<void*>(module);
#else
    void* library = ::dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (library == nullptr) {
        const char* message = ::dlerror();
        m_error = message != nullptr ? message : "dlopen failed";
        return false;
    }
    m_handle = library;
#endif
    m_error.clear();
    return true;
}

void* DynamicLibrary::Symbol(const char* name) const
{
    if (m_handle == nullptr || name == nullptr) return nullptr;
#if defined(_WIN32)
    return reinterpret_cast<void*>(::GetProcAddress(reinterpret_cast<HMODULE>(m_handle), name));
#else
    return ::dlsym(m_handle, name);
#endif
}

void DynamicLibrary::Unload()
{
    if (m_handle == nullptr) return;
#if defined(_WIN32)
    ::FreeLibrary(reinterpret_cast<HMODULE>(m_handle));
#else
    ::dlclose(m_handle);
#endif
    m_handle = nullptr;
}

} // namespace sigflow::platform
