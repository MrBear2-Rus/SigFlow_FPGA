#include "CaptureDecoder.h"
#include "DebugArtifactIO.h"
#include <json/json.h>

#include <fstream>
#include <sstream>
#include <limits>
#include <numeric>

namespace sigflow {
namespace debug {

namespace {

std::string BitsOf(std::uint32_t value, unsigned width)
{
    std::string s(width, '0');
    for (unsigned b = 0; b < width; ++b) {
        const bool bit = ((value >> b) & 1) != 0;
        s[width - 1 - b] = bit ? '1' : '0';
    }
    return s;
}

std::string LeafName(const std::string& path)
{
    const std::size_t dot = path.rfind('.');
    if (dot == std::string::npos || dot + 1 >= path.size()) return path;
    return path.substr(dot + 1);
}

// 探针 idCode：可打印非空白字符，按序分配。
std::string IdCode(int index)
{
    return std::string(1, static_cast<char>('!' + index));
}

std::uint32_t ReadU32LE(std::ifstream& in)
{
    unsigned char b[4] = {};
    in.read(reinterpret_cast<char*>(b), 4);
    return static_cast<std::uint32_t>(b[0]) |
           (static_cast<std::uint32_t>(b[1]) << 8) |
           (static_cast<std::uint32_t>(b[2]) << 16) |
           (static_cast<std::uint32_t>(b[3]) << 24);
}

} // namespace

bool DecodeCaptureToVcd(const std::vector<std::uint32_t>& samples,
                        std::uint32_t depth, std::uint16_t triggerIndex,
                        const std::vector<CaptureProbe>& probes,
                        const std::string& path, CaptureVcdMeta& meta,
                        std::string& error, const CaptureTimebase& timebase)
{
    error.clear();
    meta = CaptureVcdMeta();
    if (samples.size() < depth || probes.empty() || depth == 0) {
        error = "invalid capture input (samples<depth or no probes)";
        return false;
    }
    if (timebase.triggerObserved && triggerIndex >= depth) {
        error = "trigger index out of range";
        return false;
    }
    for (const auto& probe : probes) {
        if (probe.width == 0 || probe.width > 32 || probe.bitOffset >= 32 ||
            probe.width > 32 - probe.bitOffset) {
            error = "invalid capture probe width or bit offset"; return false;
        }
    }
    if (probes.size() > 32 || timebase.decimation == 0 || timebase.decimation > 255) {
        error = "capture requires at most 32 probes and decimation in 1..255"; return false;
    }
    meta.sampleClockHz = timebase.sampleClockHz;
    meta.decimation = timebase.decimation;
    meta.triggerObserved = timebase.triggerObserved;
    std::uint64_t wholeStep = 1, fractionalStep = 0, denominator = 1;
    if (timebase.sampleClockHz != 0) {
        const std::uint64_t numerator = 1000000000000ULL * timebase.decimation;
        denominator = timebase.sampleClockHz;
        if (denominator > numerator) { error = "sample interval is below 1ps resolution"; return false; }
        wholeStep = numerator / denominator;
        fractionalStep = numerator % denominator;
        // Preflight the final timestamp before opening/truncating the output.
        const std::uint64_t intervals = depth - 1;
        const auto limit = std::numeric_limits<std::uint64_t>::max();
        if (intervals != 0 && (wholeStep > limit / intervals ||
            fractionalStep > limit / intervals ||
            intervals * wholeStep > limit - (intervals * fractionalStep) / denominator)) {
            error = "capture time range exceeds uint64"; return false;
        }
        const auto divisor = std::gcd(numerator, denominator);
        meta.periodNumeratorPs = numerator / divisor;
        meta.periodDenominator = denominator / divisor;
        meta.timeDomain = "physical";
        meta.timescale = "1ps";
    }

    std::ofstream out(path, std::ios::trunc);
    if (!out) {
        error = "cannot open " + path;
        return false;
    }

    out << "$date\n  SigFlow TraceBridge capture\n$end\n";
    out << "$version\n  CaptureDecoder 2.0\n$end\n";
    out << "$timescale " << meta.timescale << " $end\n";
    out << "$comment sigflow_time_domain " << meta.timeDomain << " $end\n";
    out << "$comment sample_clock_hz " << meta.sampleClockHz << " decimation " << meta.decimation << " $end\n";
    out << "$comment capture_layout linear_address_order $end\n";
    out << "$comment trigger_index " << triggerIndex << " $end\n";
    // This expression is preflighted above. All samples use one absolute
    // rational origin; a rounded per-sample period is never accumulated.
    const auto tickAt = [&](std::uint64_t index) {
        return index * wholeStep + (index * fractionalStep) / denominator;
    };
    meta.lastSampleTime = tickAt(depth - 1);
    meta.triggerTime = meta.triggerObserved ? tickAt(triggerIndex) : 0;
    out << "$comment trigger_observed " << (meta.triggerObserved ? 1 : 0) << " $end\n";
    if (meta.triggerObserved) out << "$comment trigger_time " << meta.triggerTime << " $end\n";
    out << "$scope module debug $end\n";
    for (std::size_t i = 0; i < probes.size(); ++i) {
        out << "$var wire " << probes[i].width << " " << IdCode(static_cast<int>(i))
            << " " << LeafName(probes[i].path) << " $end\n";
    }
    out << "$upscope $end\n$enddefinitions $end\n";

    // Preserve the finite buffer's address order. Emit only changed values.
    std::vector<std::string> prev(probes.size());
    for (std::uint32_t t = 0; t < depth; ++t) {
        const std::uint32_t sample = samples[t];
        out << "#" << tickAt(t) << "\n";
        for (std::size_t i = 0; i < probes.size(); ++i) {
            // width == 32 时 (1u << 32) 是未定义行为；在本机 GCC 上实际得到 0，
            // 于是 32 位探针被解码成恒 0（静默错误数据）。必须单独处理。
            const std::uint32_t widthMask = probes[i].width >= 32
                ? 0xFFFFFFFFu
                : ((std::uint32_t(1) << probes[i].width) - 1);
            const std::uint32_t masked =
                (sample >> probes[i].bitOffset) & widthMask;
            const std::string value = BitsOf(masked, probes[i].width);
            if (t == 0 || value != prev[i]) {
                out << "b" << value << " " << IdCode(static_cast<int>(i)) << "\n";
                prev[i] = value;
            }
        }
    }
    out.flush();
    if (!out) {
        error = "write failed: " + path;
        return false;
    }

    return true;
}

bool SaveCaptureMetadata(const std::string& path, const CaptureVcdMeta& meta,
                         std::uint32_t depth, std::uint16_t triggerIndex, std::string& error)
{
    Json::Value root(Json::objectValue);
    root["schema_version"] = "1.0";
    root["time_domain"] = meta.timeDomain;
    root["vcd_timescale"] = meta.timescale;
    root["sample_clock_hz"] = Json::UInt64(meta.sampleClockHz);
    root["clock_source"] = meta.sampleClockHz == 0 ? "unknown" : "contract";
    root["decimation"] = meta.decimation;
    root["period_numerator_ps"] = Json::UInt64(meta.periodNumeratorPs);
    root["period_denominator"] = Json::UInt64(meta.periodDenominator);
    root["quantization"] = meta.timeDomain == "physical" ? "floor_absolute_1ps" : "sample_index";
    root["sample_enable_assumption"] = "continuous";
    root["capture_layout"] = "linear_address_order";
    root["sample_count"] = depth;
    root["trigger_index"] = triggerIndex;
    root["trigger_observed"] = meta.triggerObserved;
    root["trigger_time"] = meta.triggerObserved ? Json::Value(Json::UInt64(meta.triggerTime)) : Json::Value();
    root["last_sample_time"] = Json::UInt64(meta.lastSampleTime);
    Json::StreamWriterBuilder writer;
    writer["indentation"] = "  ";
    return WriteTextFileAtomic(path, Json::writeString(writer, root) + "\n", error);
}

bool LoadCaptureRaw(const std::string& path, std::uint32_t& depth, std::uint32_t& width,
                    std::uint16_t& start, std::vector<std::uint32_t>& samples,
                    std::string& error)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "cannot open " + path;
        return false;
    }
    char magic[8] = {};
    in.read(magic, 8);
    if (std::string(magic, 8) != "SFDBGRAW") {
        error = "bad capture.raw magic";
        return false;
    }
    const std::uint32_t version = ReadU32LE(in);
    depth = ReadU32LE(in);
    width = ReadU32LE(in);
    start = static_cast<std::uint16_t>(ReadU32LE(in));
    const std::uint32_t count = ReadU32LE(in);
    ReadU32LE(in);  // reserved
    if (version != 1 || count > 1u << 20) {
        error = "unsupported capture.raw";
        return false;
    }
    samples.clear();
    samples.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) samples.push_back(ReadU32LE(in));
    if (!in) {
        error = "truncated capture.raw";
        return false;
    }
    return true;
}

} // namespace debug
} // namespace sigflow
