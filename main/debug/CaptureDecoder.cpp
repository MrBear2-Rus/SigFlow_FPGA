#include "CaptureDecoder.h"

#include <fstream>
#include <sstream>

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
                        std::string& error)
{
    if (samples.size() < depth || probes.empty() || depth == 0) {
        error = "invalid capture input (samples<depth or no probes)";
        return false;
    }
    if (triggerIndex >= depth) {
        error = "trigger index out of range";
        return false;
    }

    std::ofstream out(path, std::ios::trunc);
    if (!out) {
        error = "cannot open " + path;
        return false;
    }

    out << "$date\n  SigFlow TraceBridge capture\n$end\n";
    out << "$version\n  CaptureDecoder 1.0\n$end\n";
    out << "$timescale " << meta.timescale << " $end\n";
    out << "$comment trigger_index " << triggerIndex << " $end\n";
    out << "$scope module debug $end\n";
    for (std::size_t i = 0; i < probes.size(); ++i) {
        out << "$var wire " << probes[i].width << " " << IdCode(static_cast<int>(i))
            << " " << LeafName(probes[i].path) << " $end\n";
    }
    out << "$upscope $end\n$enddefinitions $end\n";

    // 预计算各探针在时间轴（环形地址序）上的取值，仅输出变化点。
    std::vector<std::string> prev(probes.size());
    for (std::uint32_t t = 0; t < depth; ++t) {
        const std::uint32_t sample = samples[t];
        out << "#" << t << "\n";
        for (std::size_t i = 0; i < probes.size(); ++i) {
            const std::uint32_t masked =
                (sample >> probes[i].bitOffset) &
                ((std::uint32_t(1) << probes[i].width) - 1);
            const std::string value = BitsOf(masked, probes[i].width);
            if (t == 0 || value != prev[i]) {
                out << "b" << value << " " << IdCode(static_cast<int>(i)) << "\n";
                prev[i] = value;
            }
        }
    }
    if (!out) {
        error = "write failed: " + path;
        return false;
    }

    meta.triggerTime = triggerIndex;
    return true;
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
