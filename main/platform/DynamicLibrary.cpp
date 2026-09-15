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
    const HMODULE module = ::LoadLibraryA(path.c_str());
    if (module == nullptr) { m_error = "LoadLibraryA failed"; return false; }
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
