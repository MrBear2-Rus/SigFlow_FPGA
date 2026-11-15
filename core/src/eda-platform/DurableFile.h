#pragma once
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
namespace eda::platform {
// Exclusive writer lock. Flush is an OS durability barrier (FlushFileBuffers /
// fsync), not just a C++ stream flush. Recovery uses the same exclusive lock.
class DurableFile {
public:
    enum class Mode { CreateNew, OpenExisting };
    DurableFile(const std::filesystem::path&,Mode);
    ~DurableFile();
    void Append(std::string_view);
    void Flush();
    uint64_t Size()const;
    std::string ReadAll(size_t maximum=128*1024*1024);
    void Truncate(uint64_t);
private:
    struct Impl;std::unique_ptr<Impl> impl_;
};
}
