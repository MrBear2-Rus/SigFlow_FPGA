#include "WaveUartLane.h"

#include "../debug/DebugProtocol.h"

#include <fstream>
#include <iomanip>
#include <sstream>

namespace sigflow {
namespace wave {
namespace {

std::string Hex(const std::vector<std::uint8_t>& data)
{
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (std::size_t i = 0; i < data.size() && i < 24; ++i) {
        if (i != 0) out << ' ';
        out << std::setw(2) << static_cast<unsigned>(data[i]);
    }
    if (data.size() > 24) out << " ...";
    return out.str();
}

std::string MinimalRequestName(std::uint8_t command)
{
    switch (command) {
    case 0x10: return "CONFIG";
    case 0x11: return "ARM";
    case 0x12: return "STATUS";
    case 0x13: return "READ";
    case 0x14: return "RESET";
    default: return {};
    }
}

std::size_t MinimalRequestSize(std::uint8_t command)
{
    return command == 0x10 ? 10 : (command == 0x13 ? 4 : 2);
}

std::size_t MinimalResponseSize(std::uint8_t response)
{
    switch (response) {
    case 0x90: return 2;
    case 0x92: return 5;
    case 0x93: return 6;
    default: return 0;
    }
}

std::string MinimalResponseName(std::uint8_t response)
{
    switch (response) {
    case 0x90: return "ACK";
    case 0x92: return "STATUS";
    case 0x93: return "SAMPLES";
    default: return {};
    }
}

std::string MinimalDetail(const std::vector<std::uint8_t>& bytes,
                          std::size_t offset, std::uint8_t header,
                          std::uint8_t code, std::size_t size)
{
    std::ostringstream detail;
    detail << "minimal, " << (header == 0xA5 ? "host->fpga" : "fpga->host");
    if (header == 0xA5 && code == 0x10) {
        const std::uint32_t mask = static_cast<std::uint32_t>(bytes[offset + 2]) |
            (static_cast<std::uint32_t>(bytes[offset + 3]) << 8) |
            (static_cast<std::uint32_t>(bytes[offset + 4]) << 16) |
            (static_cast<std::uint32_t>(bytes[offset + 5]) << 24);
        const std::uint32_t value = static_cast<std::uint32_t>(bytes[offset + 6]) |
            (static_cast<std::uint32_t>(bytes[offset + 7]) << 8) |
            (static_cast<std::uint32_t>(bytes[offset + 8]) << 16) |
            (static_cast<std::uint32_t>(bytes[offset + 9]) << 24);
        detail << ", mask=0x" << std::hex << mask << ", value=0x" << value;
    } else if (header == 0xA5 && code == 0x13) {
        const std::uint16_t address = static_cast<std::uint16_t>(bytes[offset + 2]) |
            static_cast<std::uint16_t>(bytes[offset + 3]) << 8;
        detail << ", address=" << std::dec << address;
    } else if (header == 0x5A && code == 0x92) {
        detail << ", flags=0x" << std::hex << static_cast<unsigned>(bytes[offset + 2])
               << ", trigger_index=" << std::dec
               << (static_cast<unsigned>(bytes[offset + 3]) |
                   (static_cast<unsigned>(bytes[offset + 4]) << 8));
    } else if (header == 0x5A && code == 0x93) {
        const std::uint32_t sample = static_cast<std::uint32_t>(bytes[offset + 2]) |
            (static_cast<std::uint32_t>(bytes[offset + 3]) << 8) |
            (static_cast<std::uint32_t>(bytes[offset + 4]) << 16) |
            (static_cast<std::uint32_t>(bytes[offset + 5]) << 24);
        detail << ", sample=0x" << std::hex << sample;
    } else {
        detail << ", bytes=" << Hex(std::vector<std::uint8_t>(
            bytes.begin() + static_cast<std::ptrdiff_t>(offset),
            bytes.begin() + static_cast<std::ptrdiff_t>(offset + size)));
    }
    return detail.str();
}

} // namespace

const char* WaveUartLane::TypeName(std::uint8_t type)
{
    using namespace sigflow::debug::proto;
    switch (type) {
    case kT_Ping: return "PING";
    case kT_Pong: return "PONG";
    case kT_GetInfo: return "GET_INFO";
    case kT_Info: return "INFO";
    case kT_Config: return "CONFIG";
    case kT_Ack: return "ACK";
    case kT_Arm: return "ARM";
    case kT_Status: return "STATUS";
    case kT_Read: return "READ";
    case kT_Samples: return "SAMPLES";
    case kT_Reset: return "RESET";
    default: return "UNKNOWN";
    }
}

std::vector<WaveUartFrame> WaveUartLane::Decode(const std::vector<std::uint8_t>& bytes)
{
    std::vector<WaveUartFrame> result;
    std::size_t begin = 0;
    while (begin < bytes.size()) {
        while (begin < bytes.size() && bytes[begin] != 0) ++begin;
        if (begin >= bytes.size()) break;
        std::size_t end = begin + 1;
        while (end < bytes.size() && bytes[end] != 0) ++end;
        if (end >= bytes.size()) break;

        std::vector<std::uint8_t> wire(bytes.begin() + begin,
                                       bytes.begin() + end + 1);
        WaveUartFrame frame;
        frame.offset = begin;
        frame.endOffset = end;
        std::uint8_t type = 0;
        std::uint8_t sequence = 0;
        std::vector<std::uint8_t> data;
        frame.valid = sigflow::debug::proto::FrameDecode(wire, type, sequence, data);
        if (frame.valid) {
            frame.type = type;
            frame.sequence = sequence;
            frame.label = TypeName(type);
            frame.detail = "seq=" + std::to_string(sequence) +
                           ", len=" + std::to_string(data.size());
            if (!data.empty()) frame.detail += ", data=" + Hex(data);
        } else {
            frame.label = "INVALID";
            frame.detail = "COBS/CRC/frame structure error";
        }
        result.push_back(std::move(frame));
        begin = end + 1;
    }
    // 保留没有结束分隔符的尾部，避免把断链/截断误显示成“无事件”。
    std::size_t tail = bytes.size();
    while (tail > 0 && bytes[tail - 1] == 0) --tail;
    if (tail > 0 && (result.empty() || result.back().endOffset < tail)) {
        WaveUartFrame frame;
        frame.offset = result.empty() ? 0 : result.back().endOffset + 1;
        frame.endOffset = tail - 1;
        frame.label = "INVALID";
        frame.detail = "unterminated UART frame (missing 0x00 delimiter)";
        result.push_back(std::move(frame));
    }
    return result;
}

std::vector<WaveUartFrame> WaveUartLane::DecodeMinimal(const std::vector<std::uint8_t>& bytes)
{
    std::vector<WaveUartFrame> result;
    std::size_t offset = 0;
    while (offset + 1 < bytes.size()) {
        const std::uint8_t header = bytes[offset];
        if (header != 0xA5 && header != 0x5A) {
            ++offset;
            continue;
        }
        const std::uint8_t code = bytes[offset + 1];
        const std::size_t size = header == 0xA5
            ? MinimalRequestSize(code) : MinimalResponseSize(code);
        const bool recognized = header == 0xA5
            ? !MinimalRequestName(code).empty() : !MinimalResponseName(code).empty();
        if (!recognized) {
            ++offset;
            continue;
        }

        WaveUartFrame frame;
        frame.offset = offset;
        frame.endOffset = std::min(bytes.size(), offset + size) - 1;
        frame.type = code;
        frame.sequence = 0;
        frame.valid = offset + size <= bytes.size();
        frame.label = header == 0xA5 ? MinimalRequestName(code) : MinimalResponseName(code);
        if (!frame.valid) {
            frame.label = "INVALID";
            frame.detail = "truncated minimal UART frame (expected " +
                           std::to_string(size) + " bytes)";
            result.push_back(std::move(frame));
            break;
        }
        frame.detail = MinimalDetail(bytes, offset, header, code, size);
        result.push_back(std::move(frame));
        offset += size;
    }
    return result;
}

std::vector<WaveUartFrame> WaveUartLane::DecodeAuto(const std::vector<std::uint8_t>& bytes)
{
    const auto full = Decode(bytes);
    for (const auto& frame : full) {
        if (frame.valid) return full;
    }
    const auto minimal = DecodeMinimal(bytes);
    return minimal.empty() ? full : minimal;
}

std::vector<WaveUartFrame> WaveUartLane::DecodeFile(const std::string& path,
                                                   std::string& error)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "unable to open UART capture: " + path;
        return {};
    }
    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                    std::istreambuf_iterator<char>());
    if (!in.good() && !in.eof()) {
        error = "unable to read UART capture: " + path;
        return {};
    }
    error.clear();
    return DecodeAuto(bytes);
}

} // namespace wave
} // namespace sigflow
