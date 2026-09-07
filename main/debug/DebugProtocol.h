#pragma once

#include "ITransport.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace sigflow {
namespace debug {

// 分块读回采集样本的进度回调（got 已读，total 总量；可能在 I/O 线程被调用）。
using ReadProgressCb = std::function<void(std::uint32_t got, std::uint32_t total)>;

namespace proto {

// TraceBridge 链路协议（与 rtl/debug/sf_debug_link.sv 及 C++ 行为基准对齐）：
//   wire  : 0x00 | COBS(payload) | 0x00
//   payload: version(1) | type(1) | seq(1) | len(1) | data(len) | crc16_le(2)
//   CRC-16/CCITT（poly 0x1021，init 0xFFFF，MSB 先行），覆盖 version..data。

inline constexpr uint8_t kT_Ping     = 0x01;
inline constexpr uint8_t kT_Pong     = 0x02;
inline constexpr uint8_t kT_GetInfo  = 0x03;
inline constexpr uint8_t kT_Info     = 0x04;
inline constexpr uint8_t kT_Config   = 0x05;
inline constexpr uint8_t kT_Ack      = 0x06;
inline constexpr uint8_t kT_Arm      = 0x07;
inline constexpr uint8_t kT_Status   = 0x09;
inline constexpr uint8_t kT_Read     = 0x0B;
inline constexpr uint8_t kT_Samples  = 0x0C;
inline constexpr uint8_t kT_Reset    = 0x0D;

inline constexpr uint8_t kIpVersion = 1;
// 紧凑 TraceBridge 核以流式 COBS 编解码响应，每次 READ 固定返回一个采样。
// ReadCapture 会按此上限自动分块，调用者无需改变采集接口。
inline constexpr int kMaxSamples = 1;

inline uint16_t Crc16Update(uint16_t crc, uint8_t bdata)
{
    crc ^= static_cast<uint16_t>(bdata << 8);
    for (int i = 0; i < 8; ++i) {
        crc = (crc & 0x8000) ? static_cast<uint16_t>((crc << 1) ^ 0x1021)
                             : static_cast<uint16_t>(crc << 1);
    }
    return crc;
}

inline uint16_t Crc16(const std::vector<uint8_t>& data)
{
    uint16_t crc = 0xFFFF;
    for (uint8_t b : data) crc = Crc16Update(crc, b);
    return crc;
}

inline std::vector<uint8_t> CobsEncode(const std::vector<uint8_t>& payload)
{
    std::vector<uint8_t> out;
    size_t i = 0;
    while (i <= payload.size()) {
        size_t zero = i;
        while (zero < payload.size() && payload[zero] != 0) ++zero;
        out.push_back(static_cast<uint8_t>(zero - i + 1));
        for (size_t k = i; k < zero; ++k) out.push_back(payload[k]);
        i = zero + 1;
    }
    return out;
}

inline bool CobsDecode(const std::vector<uint8_t>& in, std::vector<uint8_t>& out)
{
    out.clear();
    size_t src = 0;
    while (src < in.size()) {
        uint8_t code = in[src++];
        if (code == 0) return false;
        for (uint8_t j = 1; j < code; ++j) {
            if (src >= in.size()) return false;
            out.push_back(in[src++]);
        }
        if (code < 255 && src < in.size()) out.push_back(0);
    }
    return true;
}

inline std::vector<uint8_t> MakePayload(uint8_t type, uint8_t seq,
                                        const std::vector<uint8_t>& data)
{
    std::vector<uint8_t> p = { 0x01, type, seq, static_cast<uint8_t>(data.size()) };
    p.insert(p.end(), data.begin(), data.end());
    const uint16_t crc = Crc16(p);
    p.push_back(static_cast<uint8_t>(crc & 0xFF));
    p.push_back(static_cast<uint8_t>(crc >> 8));
    return p;
}

inline std::vector<uint8_t> FrameEncode(uint8_t type, uint8_t seq,
                                        const std::vector<uint8_t>& data)
{
    std::vector<uint8_t> wire;
    wire.push_back(0x00);
    const std::vector<uint8_t> cobs = CobsEncode(MakePayload(type, seq, data));
    wire.insert(wire.end(), cobs.begin(), cobs.end());
    wire.push_back(0x00);
    return wire;
}

inline bool FrameDecode(const std::vector<uint8_t>& wire, uint8_t& type, uint8_t& seq,
                        std::vector<uint8_t>& data)
{
    if (wire.size() < 2 || wire.front() != 0 || wire.back() != 0) return false;
    const std::vector<uint8_t> body(wire.begin() + 1, wire.end() - 1);
    std::vector<uint8_t> payload;
    if (!CobsDecode(body, payload)) return false;
    if (payload.size() < 6) return false;
    if (payload[0] != 0x01) return false;
    type = payload[1];
    seq = payload[2];
    const size_t len = payload[3];
    if (payload.size() != 4 + len + 2) return false;
    const uint16_t crc =
        Crc16(std::vector<uint8_t>(payload.begin(), payload.begin() + 4 + len));
    if (crc != (payload[4 + len] | (payload[5 + len] << 8))) return false;
    data.assign(payload.begin() + 4, payload.begin() + 4 + len);
    return true;
}

} // namespace proto

// GET_INFO 返回
struct DebugInfo {
    std::uint8_t ipVersion = 0;
    std::uint64_t fingerprint64 = 0;
    std::uint16_t depth = 0;
    std::uint8_t width = 0;
    std::uint8_t flags = 0;  // bit6 done, bit5 triggered, bit4 busy
};

// STATUS 返回
struct DebugStatusInfo {
    std::uint8_t flags = 0;  // bit6 done, bit5 triggered, bit4 busy
    std::uint16_t triggerIndex = 0;
};

// 主机协议客户端（T-P3-02）：发送命令、等待同 seq 响应、有限重试、分块读取。
class DebugProtocol {
public:
    explicit DebugProtocol(ITransport& transport);

    bool Ping(std::uint8_t& version, std::string& error,
              int timeoutMs = 1000, int attempts = 3);
    bool GetInfo(DebugInfo& info, std::string& error,
                 int timeoutMs = 1000, int attempts = 3);
    bool Configure(std::uint32_t mask, std::uint32_t value, std::uint8_t decimation,
                   std::uint8_t triggerMode, std::uint16_t triggerCount,
                   std::string& error, int timeoutMs = 1000, int attempts = 3);
    bool Arm(std::string& error, int timeoutMs = 1000, int attempts = 3);
    bool Status(DebugStatusInfo& status, std::string& error,
                int timeoutMs = 1000, int attempts = 3);
    bool ReadSamples(std::uint16_t start, std::uint16_t count,
                     std::vector<std::uint32_t>& samples, std::string& error,
                     int timeoutMs = 1000, int attempts = 3);
    // 分块读取（每块 ≤ kMaxSamples），失败时报告缺失区间。
    bool ReadCapture(std::uint16_t start, std::uint16_t count,
                     std::vector<std::uint32_t>& samples, std::string& error,
                     int timeoutMs = 1000, int attempts = 3);
    // 带进度回调的 ReadCapture（progressCb 为 nullptr 时等价于上一版）。
    bool ReadCapture(std::uint16_t start, std::uint16_t count,
                     std::vector<std::uint32_t>& samples, std::string& error,
                     int timeoutMs, int attempts, const ReadProgressCb& progressCb);
    bool Reset(std::string& error, int timeoutMs = 1000, int attempts = 3);
    // 同步校准：0x55/0xAA 前导 + PING 校验链路就绪（T-P3-01 剩项）。
    bool SyncCalibrate(std::string& error, int timeoutMs = 1000, int attempts = 2);
    // 断链后重连：重新 Open + 同步校准（自动重连开关见 SetAutoReconnect）。
    bool Reconnect(std::string& error);
    void SetAutoReconnect(bool enabled) { autoReconnect_ = enabled; }

private:
    bool RoundTripInternal(std::uint8_t type, const std::vector<std::uint8_t>& data,
                           std::uint8_t& respType, std::vector<std::uint8_t>& respData,
                           std::string& error, int timeoutMs, int attempts);
    bool RoundTrip(std::uint8_t type, const std::vector<std::uint8_t>& data,
                   std::uint8_t& respType, std::vector<std::uint8_t>& respData,
                   std::string& error, int timeoutMs, int attempts);

    ITransport& transport_;
    std::uint8_t seq_ = 0;
    bool autoReconnect_ = false;
};

// 专用 UART 的最小固定帧协议客户端，对应 sf_debug_link_minimal.sv。
// 帧格式没有 COBS/CRC，仅适合 dbg_rx/dbg_tx 不与用户业务复用的场景。
class MinimalDebugProtocol {
public:
    explicit MinimalDebugProtocol(ITransport& transport);

    bool Configure(std::uint32_t mask, std::uint32_t value, std::string& error,
                   int timeoutMs = 1000, int attempts = 3);
    bool Arm(std::string& error, int timeoutMs = 1000, int attempts = 3);
    bool Status(DebugStatusInfo& status, std::string& error,
                int timeoutMs = 1000, int attempts = 3);
    bool ReadCapture(std::uint16_t start, std::uint16_t count,
                     std::vector<std::uint32_t>& samples, std::string& error,
                     int timeoutMs = 1000, int attempts = 3);
    bool ReadCapture(std::uint16_t start, std::uint16_t count,
                     std::vector<std::uint32_t>& samples, std::string& error,
                     int timeoutMs, int attempts, const ReadProgressCb& progressCb);
    bool Reset(std::string& error, int timeoutMs = 1000, int attempts = 3);
    void SetAutoReconnect(bool enabled) { autoReconnect_ = enabled; }

private:
    bool Command(std::uint8_t command, const std::vector<std::uint8_t>& data,
                 std::uint8_t expectedResponse, std::size_t expectedSize,
                 std::vector<std::uint8_t>& response, std::string& error,
                 int timeoutMs, int attempts);
    bool Reconnect(std::string& error);

    ITransport& transport_;
    bool autoReconnect_ = false;
};

// capture.raw 保存（T-P3-02；与 Loopback golden 格式一致）：
//   magic "SFDBGRAW" | version u32 | depth u32 | width u32 | start u32 | count u32
//   | reserved u32 | samples count×width/8 字节（小端）
bool SaveCaptureRaw(const std::string& path, std::uint32_t depth, std::uint32_t width,
                    std::uint16_t start, const std::vector<std::uint32_t>& samples,
                    std::string& error);

} // namespace debug
} // namespace sigflow
