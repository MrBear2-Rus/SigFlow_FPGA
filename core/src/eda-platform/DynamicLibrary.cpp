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

namespace eda {
namespace platform {

DynamicLibrary::~DynamicLibrary() { Unload(); }

bool DynamicLibrary::Load(const std::string& path) {
    Unload();
#if defined(_WIN32)
    const int wideLength = ::MultiByteToWideChar(
        CP_UTF8, 0, path.c_str(), static_cast<int>(path.size()), nullptr, 0);
    if (wideLength <= 0) {
        error_ = "plugin path is not valid UTF-8";
        return false;
    }
    std::wstring widePath(static_cast<std::size_t>(wideLength), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, path.c_str(), static_cast<int>(path.size()),
                          widePath.data(), wideLength);
    const HMODULE module = ::LoadLibraryW(widePath.c_str());
    if (module == nullptr) {
        error_ = "LoadLibraryW failed (Win32 error " + std::to_string(::GetLastError()) + ")";
        return false;
    }
    handle_ = reinterpret_cast<void*>(module);
#else
    void* library = ::dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (library == nullptr) {
        const char* message = ::dlerror();
        error_ = message != nullptr ? message : "dlopen failed";
        return false;
    }
    handle_ = library;
#endif
    error_.clear();
    return true;
}

void* DynamicLibrary::Symbol(const char* name) const {
    if (handle_ == nullptr || name == nullptr) return nullptr;
#if defined(_WIN32)
    return reinterpret_cast<void*>(::GetProcAddress(reinterpret_cast<HMODULE>(handle_), name));
#else
    return ::dlsym(handle_, name);
#endif
}

void DynamicLibrary::Unload() {
    if (handle_ == nullptr) return;
#if defined(_WIN32)
    ::FreeLibrary(reinterpret_cast<HMODULE>(handle_));
#else
    ::dlclose(handle_);
#endif
    handle_ = nullptr;
}

} // namespace platform
} // namespace eda
