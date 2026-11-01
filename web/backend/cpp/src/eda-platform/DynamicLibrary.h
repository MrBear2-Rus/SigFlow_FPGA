#pragma once

#include <string>

namespace eda {
namespace platform {

// 平台动态库加载：Windows=LoadLibraryW/GetProcAddress/FreeLibrary，POSIX=dlopen/dlsym/dlclose。
class DynamicLibrary {
public:
    DynamicLibrary() = default;
    ~DynamicLibrary();
    DynamicLibrary(const DynamicLibrary&) = delete;
    DynamicLibrary& operator=(const DynamicLibrary&) = delete;

    // path 约定为 UTF-8（Windows 用 LoadLibraryW，避免 ANSI 代码页丢失非 ASCII 安装路径）。
    bool Load(const std::string& path);
    void* Symbol(const char* name) const;
    void Unload();
    bool IsLoaded() const { return handle_ != nullptr; }
    const std::string& LastError() const { return error_; }

private:
    void* handle_ = nullptr;
    std::string error_;
};

} // namespace platform
} // namespace eda
