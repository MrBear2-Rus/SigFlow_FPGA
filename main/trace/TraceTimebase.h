#pragma once
#include <cctype>
#include <cstdint>
#include <string>

namespace sigflow::trace {
enum class TraceTimeDomain { Unknown, Physical, SampleIndex };

inline bool VcdTickFemtoseconds(std::string scale, std::uint64_t& value)
{
    std::string compact;
    for (unsigned char c : scale) if (!std::isspace(c)) compact += static_cast<char>(c);
    std::size_t n = 0;
    while (n < compact.size() && compact[n] >= '0' && compact[n] <= '9') ++n;
    const auto digits = compact.substr(0, n);
    if (digits != "1" && digits != "10" && digits != "100") return false;
    const auto unit = compact.substr(n);
    std::uint64_t factor = 0;
    if (unit == "fs") factor = 1;
    else if (unit == "ps") factor = 1000;
    else if (unit == "ns") factor = 1000000;
    else if (unit == "us") factor = 1000000000;
    else if (unit == "ms") factor = 1000000000000ULL;
    else if (unit == "s") factor = 1000000000000000ULL;
    else return false;
    value = factor * (digits == "1" ? 1 : digits == "10" ? 10 : 100);
    return true;
}

inline TraceTimeDomain ClassifyVcdTime(const std::string& header, const std::string& scale)
{
    if (header.find("CaptureDecoder 1.0") != std::string::npos ||
        header.find("sigflow_time_domain sample_index") != std::string::npos)
        return TraceTimeDomain::SampleIndex;
    std::uint64_t tick = 0;
    return VcdTickFemtoseconds(scale, tick) ? TraceTimeDomain::Physical : TraceTimeDomain::Unknown;
}
} // namespace sigflow::trace
