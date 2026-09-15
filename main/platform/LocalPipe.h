#pragma once
#include <cstdint>
#include <cstddef>
#include <memory>
#include <string>
namespace sigflow::platform {
class LocalPipe {
public:
    LocalPipe(std::string name, bool server);
    ~LocalPipe();
    LocalPipe(const LocalPipe&) = delete;
    LocalPipe& operator=(const LocalPipe&) = delete;
    bool Open();
    void Close();
    bool IsOpen() const;
    std::size_t Read(std::uint8_t* data, std::size_t size, int timeoutMs);
    bool Write(const std::uint8_t* data, std::size_t size, int timeoutMs);
    static bool CreatePair(std::string name, std::unique_ptr<LocalPipe>& hostEnd,
                           std::unique_ptr<LocalPipe>& deviceEnd, std::string& error);
private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
} // namespace sigflow::platform
