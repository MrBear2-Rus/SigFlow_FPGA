#include "DynamicLibrary.h"

#include <dlfcn.h>

namespace eda {
namespace platform {

DynamicLibrary::~DynamicLibrary() { Unload(); }

bool DynamicLibrary::Load(const std::string& path) {
    Unload();
    void* library = ::dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (library == nullptr) {
        const char* message = ::dlerror();
        error_ = message != nullptr ? message : "dlopen failed";
        return false;
    }
    handle_ = library;
    error_.clear();
    return true;
}

void* DynamicLibrary::Symbol(const char* name) const {
    if (handle_ == nullptr || name == nullptr) return nullptr;
    return ::dlsym(handle_, name);
}

void DynamicLibrary::Unload() {
    if (handle_ == nullptr) return;
    ::dlclose(handle_);
    handle_ = nullptr;
}

} // namespace platform
} // namespace eda
