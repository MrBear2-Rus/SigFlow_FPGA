#include "DebugProtocol.h"

#include <chrono>
#include <fstream>

namespace sigflow {
namespace debug {

namespace {

using proto::FrameDecode;
using proto::FrameEncode;
using proto::kT_Ack;
using proto::kT_Info;
using proto::kT_Read;
using proto::kT_Samples;
using proto::kT_Status;

// 从传输读出一条完整 wire 帧（0x00 ... 0x00）；超时/断连返回 false。
bool ReceiveWireFrame(ITransport& transport, std::vector<std::uint8_t>& frame,
                      int timeoutMs, std::string& error)
{
    std::vector<std::uint8_t> acc;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    std::uint8_t buf[512];
    while (true) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            error = "response timeout";
            return false;
        }
        int remain = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());
        if (remain < 1) remain = 1;
        const std::size_t n = transport.Read(buf, sizeof(buf), remain);
        if (n == 0) {
            if (!transport.IsOpen()) {
                error = "link lost";
                return false;
            }
            continue;
        }
        acc.insert(acc.end(), buf, buf + n);

        // 丢弃首个 0x00 前的噪声
        std::size_t first = acc.size();
        for (std::size_t i = 0; i < acc.size(); ++i) {
            if (acc[i] == 0x00) {
                first = i;
                break;
            }
        }
        if (first > 0) acc.erase(acc.begin(), acc.begin() + first);
        if (acc.size() >= 2 && acc[0] == 0x00) {
            for (std::size_t j = 1; j < acc.size(); ++j) {
                if (acc[j] == 0x00) {
                    frame.assign(acc.begin(), acc.begin() + j + 1);
                    return true;
                }
            }
        }
    }
}

void AppendU16LE(std::vector<std::uint8_t>& out, std::uint16_t v)
{
    out.push_back(static_cast<std::uint8_t>(v & 0xFF));
    out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
}

bool ReadExact(ITransport& transport, std::uint8_t* data, std::size_t size,
               int timeoutMs, std::string& error)
{
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    std::size_t received = 0;
    while (received < size) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            error = "response timeout";
            return false;
        }
        int remain = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());
        if (remain < 1) remain = 1;
        const std::size_t n = transport.Read(data + received, size - received, remain);
        if (n == 0) {
            if (!transport.IsOpen()) {
                error = "link lost";
                return false;
            }
            continue;
        }
        received += n;
    }
    return true;
}

} // namespace

DebugProtocol::DebugProtocol(ITransport& transport) : transport_(transport)
{
}

bool DebugProtocol::RoundTrip(std::uint8_t type, const std::vector<std::uint8_t>& data,
                              std::uint8_t& respType,
                              std::vector<std::uint8_t>& respData, std::string& error,
                              int timeoutMs, int attempts)
{
    if (autoReconnect_ && !transport_.IsOpen()) {
        if (!Reconnect(error)) return false;
    }
    return RoundTripInternal(type, data, respType, respData, error, timeoutMs, attempts);
}

bool DebugProtocol::RoundTripInternal(std::uint8_t type,
                                      const std::vector<std::uint8_t>& data,
                                      std::uint8_t& respType,
                                      std::vector<std::uint8_t>& respData,
                                      std::string& error, int timeoutMs, int attempts)
{
    for (int attempt = 0; attempt < attempts; ++attempt) {
        const std::uint8_t seq = ++seq_;
        const auto wire = proto::FrameEncode(type, seq, data);
        if (!transport_.Write(wire.data(), wire.size(), timeoutMs)) {
            error = transport_.IsOpen() ? "write failed" : "link lost";
            return false;
        }

        std::vector<std::uint8_t> frame;
        if (!ReceiveWireFrame(transport_, frame, timeoutMs, error)) {
            if (!transport_.IsOpen()) return false;  // 断连不再重试
            continue;  // 超时：重发同 seq
        }

        std::uint8_t rseq = 0;
        if (!proto::FrameDecode(frame, respType, rseq, respData)) {
            // 收到坏帧（CRC/结构错误）：视为噪声，继续等待本命令的响应。
            continue;
        }
        if (rseq != seq) {
            // 序号不匹配：继续等待（响应可能粘包/乱序）。
            continue;
        }
        return true;
    }
    if (error.empty()) error = "no valid response after retries";
    return false;
}

bool DebugProtocol::SyncCalibrate(std::string& error, int timeoutMs, int attempts)
{
    // 0x55/0xAA 前导：RTL RX_IDLE 忽略非 0x00 字节，模型同样丢弃，随后 PING 验证链路。
    const std::vector<std::uint8_t> preamble = { 0x55, 0xAA, 0x55, 0xAA };
    if (!transport_.Write(preamble.data(), preamble.size(), timeoutMs)) {
        error = transport_.IsOpen() ? "sync preamble write failed" : "link lost";
        return false;
    }
    std::uint8_t version = 0;
    return Ping(version, error, timeoutMs, attempts);
}

bool DebugProtocol::Reconnect(std::string& error)
{
    transport_.Close();
    if (!transport_.Open()) {
        error = "reconnect open failed";
        return false;
    }
    return SyncCalibrate(error);
}

bool DebugProtocol::Ping(std::uint8_t& version, std::string& error,
                         int timeoutMs, int attempts)
{
    std::uint8_t respType = 0;
    std::vector<std::uint8_t> data;
    if (!RoundTrip(proto::kT_Ping, { 0x07 }, respType, data, error, timeoutMs, attempts))
        return false;
    if (respType != proto::kT_Pong || data.size() != 1) {
        error = "unexpected PING response";
        return false;
    }
    version = data[0];
    return true;
}

bool DebugProtocol::GetInfo(DebugInfo& info, std::string& error,
                            int timeoutMs, int attempts)
{
    std::uint8_t respType = 0;
    std::vector<std::uint8_t> data;
    if (!RoundTrip(proto::kT_GetInfo, {}, respType, data, error, timeoutMs, attempts))
        return false;
    if (respType != kT_Info || data.size() != 13) {
        error = "unexpected GET_INFO response";
        return false;
    }
    info.ipVersion = data[0];
    info.fingerprint64 = 0;
    for (int n = 0; n < 8; ++n)
        info.fingerprint64 |= static_cast<std::uint64_t>(data[1 + n]) << (n * 8);
    info.depth = static_cast<std::uint16_t>(data[9] | (data[10] << 8));
    info.width = data[11];
    info.flags = data[12];
    return true;
}

bool DebugProtocol::Configure(std::uint32_t mask, std::uint32_t value,
                              std::uint8_t decimation, std::uint8_t triggerMode,
                              std::uint16_t triggerCount, std::string& error,
                              int timeoutMs, int attempts)
{
    std::vector<std::uint8_t> data;
    for (int b = 0; b < 4; ++b) {
        data.push_back(static_cast<std::uint8_t>((mask >> (b * 8)) & 0xFF));
    }
    for (int b = 0; b < 4; ++b) {
        data.push_back(static_cast<std::uint8_t>((value >> (b * 8)) & 0xFF));
    }
    data.push_back(decimation);
    data.push_back(triggerMode & 0x03);
    AppendU16LE(data, triggerCount);

    std::uint8_t respType = 0;
    std::vector<std::uint8_t> respData;
    if (!RoundTrip(proto::kT_Config, data, respType, respData, error, timeoutMs, attempts))
        return false;
    if (respType != kT_Ack) {
        error = "unexpected CONFIG response";
        return false;
    }
    return true;
}

bool DebugProtocol::Arm(std::string& error, int timeoutMs, int attempts)
{
    std::uint8_t respType = 0;
    std::vector<std::uint8_t> respData;
    if (!RoundTrip(proto::kT_Arm, {}, respType, respData, error, timeoutMs, attempts))
        return false;
    if (respType != kT_Ack) {
        error = "unexpected ARM response";
        return false;
    }
    return true;
}

bool DebugProtocol::Status(DebugStatusInfo& status, std::string& error,
                           int timeoutMs, int attempts)
{
    std::uint8_t respType = 0;
    std::vector<std::uint8_t> data;
    if (!RoundTrip(proto::kT_Status, {}, respType, data, error, timeoutMs, attempts))
        return false;
    if (respType != kT_Status || data.size() < 3) {
        error = "unexpected STATUS response";
        return false;
    }
    status.flags = data[0];
    status.triggerIndex = static_cast<std::uint16_t>(data[1] | (data[2] << 8));
    return true;
}

bool DebugProtocol::ReadSamples(std::uint16_t start, std::uint16_t count,
                                std::vector<std::uint32_t>& samples, std::string& error,
                                int timeoutMs, int attempts)
{
    if (count != proto::kMaxSamples) {
        error = "READ supports exactly " + std::to_string(proto::kMaxSamples) +
                " sample; use ReadCapture for a range";
        return false;
    }
    std::vector<std::uint8_t> data;
    AppendU16LE(data, start);
    AppendU16LE(data, count);

    std::uint8_t respType = 0;
    std::vector<std::uint8_t> respData;
    if (!RoundTrip(kT_Read, data, respType, respData, error, timeoutMs, attempts))
        return false;
    if (respType != kT_Samples || respData.size() < 4) {
        error = "unexpected READ response";
        return false;
    }
    const std::uint16_t gotStart =
        static_cast<std::uint16_t>(respData[0] | (respData[1] << 8));
    const std::uint16_t gotCount =
        static_cast<std::uint16_t>(respData[2] | (respData[3] << 8));
    if (gotStart != start || gotCount != count || respData.size() != 4 + gotCount * 4) {
        error = "READ response range mismatch";
        return false;
    }
    samples.clear();
    samples.reserve(gotCount);
    for (int i = 0; i < gotCount; ++i) {
        std::uint32_t s = 0;
        for (int b = 0; b < 4; ++b)
            s |= static_cast<std::uint32_t>(respData[4 + i * 4 + b]) << (b * 8);
        samples.push_back(s);
    }
    return true;
}

bool DebugProtocol::ReadCapture(std::uint16_t start, std::uint16_t count,
                                std::vector<std::uint32_t>& samples, std::string& error,
                                int timeoutMs, int attempts)
{
    samples.clear();
    samples.reserve(count);
    std::uint16_t pos = start;
    while (pos < start + count) {
        const std::uint16_t chunk =
            static_cast<std::uint16_t>(std::min<std::size_t>(proto::kMaxSamples,
                                                             start + count - pos));
        std::vector<std::uint32_t> part;
        if (!ReadSamples(pos, chunk, part, error, timeoutMs, attempts)) {
            error = "read chunk [" + std::to_string(pos) + "," +
                    std::to_string(pos + chunk) + ") failed: " + error;
            return false;
        }
        samples.insert(samples.end(), part.begin(), part.end());
        pos = static_cast<std::uint16_t>(pos + chunk);
    }
    return true;
}

bool DebugProtocol::Reset(std::string& error, int timeoutMs, int attempts)
{
    std::uint8_t respType = 0;
    std::vector<std::uint8_t> respData;
    if (!RoundTrip(proto::kT_Reset, {}, respType, respData, error, timeoutMs, attempts))
        return false;
    if (respType != kT_Ack) {
        error = "unexpected RESET response";
        return false;
    }
    return true;
}

MinimalDebugProtocol::MinimalDebugProtocol(ITransport& transport) : transport_(transport)
{
}

bool MinimalDebugProtocol::Reconnect(std::string& error)
{
    transport_.Close();
    if (!transport_.Open()) {
        error = "reconnect open failed";
        return false;
    }
    return true;
}

bool MinimalDebugProtocol::Command(std::uint8_t command,
                                   const std::vector<std::uint8_t>& data,
                                   std::uint8_t expectedResponse,
                                   std::size_t expectedSize,
                                   std::vector<std::uint8_t>& response,
                                   std::string& error, int timeoutMs, int attempts)
{
    if (autoReconnect_ && !transport_.IsOpen() && !Reconnect(error)) return false;
    std::vector<std::uint8_t> request = { 0xA5, command };
    request.insert(request.end(), data.begin(), data.end());
    for (int attempt = 0; attempt < attempts; ++attempt) {
        if (!transport_.Write(request.data(), request.size(), timeoutMs)) {
            error = transport_.IsOpen() ? "write failed" : "link lost";
            return false;
        }

        // Resynchronize on the two-byte response header.  A fixed command is
        // issued only after the previous reply was completely consumed.
        std::uint8_t byte = 0;
        bool header = false;
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(timeoutMs);
        while (std::chrono::steady_clock::now() < deadline) {
            const auto now = std::chrono::steady_clock::now();
            int remain = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - now).count());
            if (remain < 1) remain = 1;
            if (!ReadExact(transport_, &byte, 1, remain, error)) break;
            if (!header) {
                header = byte == 0x5A;
            } else if (byte == expectedResponse) {
                response.assign(expectedSize, 0);
                if (expectedSize == 0 || ReadExact(transport_, response.data(), expectedSize,
                                                   remain, error)) {
                    return true;
                }
                break;
            } else {
                header = byte == 0x5A;
            }
        }
        if (!transport_.IsOpen()) return false;
    }
    if (error.empty()) error = "no valid response after retries";
    return false;
}

bool MinimalDebugProtocol::Configure(std::uint32_t mask, std::uint32_t value,
                                     std::string& error, int timeoutMs, int attempts)
{
    std::vector<std::uint8_t> data;
    for (int b = 0; b < 4; ++b) data.push_back(static_cast<std::uint8_t>(mask >> (b * 8)));
    for (int b = 0; b < 4; ++b) data.push_back(static_cast<std::uint8_t>(value >> (b * 8)));
    std::vector<std::uint8_t> response;
    return Command(0x10, data, 0x90, 0, response, error, timeoutMs, attempts);
}

bool MinimalDebugProtocol::Arm(std::string& error, int timeoutMs, int attempts)
{
    std::vector<std::uint8_t> response;
    return Command(0x11, {}, 0x90, 0, response, error, timeoutMs, attempts);
}

bool MinimalDebugProtocol::Status(DebugStatusInfo& status, std::string& error,
                                  int timeoutMs, int attempts)
{
    std::vector<std::uint8_t> response;
    if (!Command(0x12, {}, 0x92, 3, response, error, timeoutMs, attempts)) return false;
    status.flags = response[0];
    status.triggerIndex = static_cast<std::uint16_t>(response[1] | (response[2] << 8));
    return true;
}

bool MinimalDebugProtocol::ReadCapture(std::uint16_t start, std::uint16_t count,
                                       std::vector<std::uint32_t>& samples,
                                       std::string& error, int timeoutMs, int attempts)
{
    samples.clear();
    samples.reserve(count);
    for (std::uint32_t address = start; address < static_cast<std::uint32_t>(start) + count;
         ++address) {
        std::vector<std::uint8_t> request;
        AppendU16LE(request, static_cast<std::uint16_t>(address));
        std::vector<std::uint8_t> response;
        if (!Command(0x13, request, 0x93, 4, response, error, timeoutMs, attempts)) {
            error = "read sample " + std::to_string(address) + " failed: " + error;
            return false;
        }
        std::uint32_t sample = 0;
        for (int b = 0; b < 4; ++b)
            sample |= static_cast<std::uint32_t>(response[b]) << (b * 8);
        samples.push_back(sample);
    }
    return true;
}

bool MinimalDebugProtocol::Reset(std::string& error, int timeoutMs, int attempts)
{
    std::vector<std::uint8_t> response;
    return Command(0x14, {}, 0x90, 0, response, error, timeoutMs, attempts);
}

bool SaveCaptureRaw(const std::string& path, std::uint32_t depth, std::uint32_t width,
                    std::uint16_t start, const std::vector<std::uint32_t>& samples,
                    std::string& error)
{
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        error = "cannot open " + path;
        return false;
    }
    const auto writeU32 = [&out](std::uint32_t v) {
        const char bytes[4] = {
            static_cast<char>(v & 0xFF), static_cast<char>((v >> 8) & 0xFF),
            static_cast<char>((v >> 16) & 0xFF), static_cast<char>((v >> 24) & 0xFF),
        };
        out.write(bytes, 4);
    };
    out.write("SFDBGRAW", 8);
    writeU32(1);  // version
    writeU32(depth);
    writeU32(width);
    writeU32(start);
    writeU32(static_cast<std::uint32_t>(samples.size()));
    writeU32(0);  // reserved
    for (std::uint32_t s : samples) writeU32(s);
    if (!out) {
        error = "write failed: " + path;
        return false;
    }
    return true;
}

} // namespace debug
} // namespace sigflow
