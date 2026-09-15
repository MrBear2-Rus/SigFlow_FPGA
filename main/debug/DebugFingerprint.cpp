#include "DebugFingerprint.h"

#include "../jobs/Sha256.h"

#include <sstream>

namespace sigflow {
namespace debug {

std::string Sha256String(const std::string& data)
{
    return std::string(Sha256Hex(data.data(), data.size()).ToStdString());
}

std::string Sha256File(const std::string& path)
{
    return std::string(Sha256FileHex(wxString::FromUTF8(path)).ToStdString());
}

std::string ComputeSourceFingerprint(const DebugFingerprintInputs& inputs)
{
    std::ostringstream stream;
    for (const std::string& file : inputs.sourceFiles) {
        const std::string hash = Sha256File(file);
        stream << file << ":" << hash << "\n";
    }
    stream << "top=" << inputs.topModule << "\n";
    stream << "constraints=" << Sha256String(inputs.constraintsContent) << "\n";
    stream << "target=" << inputs.targetProfile << "\n";
    stream << "probes=" << Sha256String(inputs.probeSignature) << "\n";
    return Sha256String(stream.str());
}

std::string ComputeToolchainFingerprint(const std::vector<std::string>& toolVersions,
                                        const std::string& debugIpVersion)
{
    std::ostringstream stream;
    for (const std::string& version : toolVersions) {
        stream << version << "\n";
    }
    stream << "debug_ip=" << debugIpVersion << "\n";
    return Sha256String(stream.str());
}

std::uint64_t Fingerprint64FromHex(const std::string& sha256Hex)
{
    std::uint64_t value = 0;
    // 取最后 16 个 hex 字符
    const std::size_t start = sha256Hex.size() >= 16 ? sha256Hex.size() - 16 : 0;
    for (std::size_t i = start; i < sha256Hex.size(); ++i) {
        const char c = sha256Hex[i];
        unsigned nibble = 0;
        if (c >= '0' && c <= '9') nibble = static_cast<unsigned>(c - '0');
        else if (c >= 'a' && c <= 'f') nibble = static_cast<unsigned>(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') nibble = static_cast<unsigned>(c - 'A' + 10);
        else continue;
        value = (value << 4) | nibble;
    }
    return value;
}

} // namespace debug
} // namespace sigflow
