#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace sigflow {
namespace debug {

struct DebugFingerprintInputs {
    std::vector<std::string> sourceFiles;      // RTL 文件路径
    std::string topModule;
    std::string constraintsContent;            // CST 内容（或空）
    std::string targetProfile;
    std::string probeSignature;                // 探针排序/位宽/触发参数的规范化串
};

// P0-07 指纹方案。
std::string Sha256String(const std::string& data);
std::string Sha256File(const std::string& path);

// 源指纹：RTL 内容哈希 + 顶层模块 + 约束 + 目标板 + 探针签名。
std::string ComputeSourceFingerprint(const DebugFingerprintInputs& inputs);

// 工具链指纹：Yosys/nextpnr/gowin_pack 版本 + 调试核版本。
std::string ComputeToolchainFingerprint(const std::vector<std::string>& toolVersions,
                                        const std::string& debugIpVersion);

// 取 sha256 十六进制串的低 64 bit（GET_INFO 返回）。
std::uint64_t Fingerprint64FromHex(const std::string& sha256Hex);

} // namespace debug
} // namespace sigflow
