#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace sigflow {
namespace debug {

// 探针（T-P0-01/05）。
struct DebugProbe {
    std::string id;
    std::string path;         // 层级路径，如 uart_top.tx_busy
    unsigned width = 1;
    unsigned bitOffset = 0;
    std::string clockDomain;  // 空 = 未推断
    bool asynchronous = false; // 明确标记为异步观察时允许跨域，但结果降低置信度
};

struct DebugClockDomain {
    std::string id;
    std::string signal;
    std::uint64_t frequencyHz = 0;
};

struct DebugSampleClock {
    std::string signal;
    std::uint64_t frequencyHz = 0;
};

struct DebugTrigger {
    std::string kind;         // none / mask_equal / edge_rising / edge_falling / comb_and
    std::string mask;         // 十六进制掩码
    std::string value;        // 十六进制比较值
    // 意图模板（T-P0-01，16.1 扩展预留）
    std::string intentKind;   // state_stall / handshake_timeout / fifo_error / illegal_state
    std::string intentParams; // 原始参数 JSON 串
    std::string expanded;     // 展开后的底层条件说明
    // 握手超时触发的信号来源（空 = 不接线，核内默认 hs_valid=0 / hs_ready=1）
    std::string hsValidPath;
    std::string hsReadyPath;
};

struct DebugCapture {
    std::uint32_t depth = 0;            // 0 = 使用默认值
    std::uint32_t pretriggerSamples = 0; // 0 = 使用默认值
    std::uint32_t decimation = 1;
};

struct DebugTransport {
    std::string kind = "uart";
    // minimal：专用固定帧 UART（默认）；full：COBS + CRC 通用协议。
    std::string protocol = "minimal";
    std::string txPort;
    std::string rxPort;
    std::uint32_t baud = 0;             // 0 = 使用默认值
    bool syncEnabled = true;            // 0x55/0xAA 同步校准（T-P0-06）
    int txPin = 0;                      // 0 = 未分配（由引脚面板/用户 CST 提供）
    int rxPin = 0;
    int rstPin = 0;
};

struct DebugFingerprints {
    std::string source;       // 源/约束/探针/参数指纹（sha256 hex）
    std::string toolchain;    // 工具链 + 调试核版本指纹
    std::string bitstream;    // .fs 字节哈希（可选）
    std::uint64_t fingerprint64 = 0;    // GET_INFO 返回的低 64 bit
};

// 调试契约（debug-contract.json，T-P0-01）。
struct DebugContract {
    std::string schemaVersion = "1.0";
    std::string sessionId;
    std::string targetProfile;
    std::string topModule;
    DebugSampleClock sampleClock;
    std::vector<DebugClockDomain> clockDomains;
    std::vector<DebugProbe> probes;
    DebugTrigger trigger;
    DebugCapture capture;
    DebugTransport transport;
    DebugFingerprints fingerprints;

    // 解析 JSON（缺失字段填默认值）；失败返回 false 并给出错误。
    bool ParseJson(const std::string& json, std::string& error);
    // 序列化为 JSON。
    std::string ToJson() const;
    // 字段完整性校验（不校验指纹值本身）。
    bool Validate(std::string& error) const;
    // 应用默认值（P0-04 阈值表）。
    void ApplyDefaults();
    // 自动分配 bit_offset 并检查总宽度（P0-05 探针分组）。
    bool AssignProbeBitOffsets(std::string& error);
    std::vector<std::string> EffectiveProbeClockDomains() const;
    bool UsesMultipleClockDomains() const;
};

// 生成新会话 ID（时间戳 + 序号）。
std::string NewDebugSessionId();

// 解析十六进制（支持 0x 前缀），失败返回 false。
bool ParseHexU32(const std::string& text, std::uint32_t& value);

} // namespace debug
} // namespace sigflow
