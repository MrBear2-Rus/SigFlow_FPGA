#pragma once
#include <string>

namespace sigflow::platform {

// 平台动态库加载：Windows=LoadLibrary/GetProcAddress/FreeLibrary，POSIX=dlopen/dlsym/dlclose。
class DynamicLibrary {
public:
    DynamicLibrary() = default;
    ~DynamicLibrary();
    DynamicLibrary(const DynamicLibrary&) = delete;
    DynamicLibrary& operator=(const DynamicLibrary&) = delete;

    bool Load(const std::string& path);
    void* Symbol(const char* name) const;
    void Unload();
    bool IsLoaded() const { return m_handle != nullptr; }
    const std::string& LastError() const { return m_error; }

private:
    void* m_handle = nullptr;   // HMODULE (Win) / void* (POSIX)
    std::string m_error;
};

} // namespace sigflow::platform
