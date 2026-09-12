#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace sigflow {
namespace wave {

struct WaveUartFrame {
    std::size_t offset = 0;
    std::size_t endOffset = 0;
    std::uint8_t type = 0;
    std::uint8_t sequence = 0;
    bool valid = false;
    std::string label;
    std::string detail;
};

class WaveUartLane {
public:
    static std::vector<WaveUartFrame> Decode(const std::vector<std::uint8_t>& bytes);
    static std::vector<WaveUartFrame> DecodeMinimal(const std::vector<std::uint8_t>& bytes);
    static std::vector<WaveUartFrame> DecodeAuto(const std::vector<std::uint8_t>& bytes);
    static std::vector<WaveUartFrame> DecodeFile(const std::string& path,
                                                 std::string& error);
    static const char* TypeName(std::uint8_t type);
};

} // namespace wave
} // namespace sigflow
