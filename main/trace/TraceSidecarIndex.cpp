#include "TraceSidecarIndex.h"

#include <fstream>
#include <limits>

namespace sigflow {
namespace trace {

namespace {

bool WriteString(std::ostream& out, const std::string& value)
{
    const std::uint32_t length = static_cast<std::uint32_t>(value.size());
    if (!out.write(reinterpret_cast<const char*>(&length), sizeof(length))) return false;
    if (length > 0 && !out.write(value.data(), length)) return false;
    return true;
}

bool ReadString(std::istream& in, std::string& value)
{
    std::uint32_t length = 0;
    if (!in.read(reinterpret_cast<char*>(&length), sizeof(length))) return false;
    if (length > (1u << 20)) return false; // 防分配炸弹
    value.resize(length);
    if (length > 0 && !in.read(&value[0], length)) return false;
    return true;
}

template <typename T>
bool WritePod(std::ostream& out, const T& value)
{
    return static_cast<bool>(out.write(reinterpret_cast<const char*>(&value), sizeof(value)));
}

template <typename T>
bool ReadPod(std::istream& in, T& value)
{
    return static_cast<bool>(in.read(reinterpret_cast<char*>(&value), sizeof(value)));
}

} // namespace

std::string TraceSidecarIndex::SidecarPathFor(const std::string& vcdPath)
{
    return vcdPath + ".bwidx";
}

bool TraceSidecarIndex::Save(const std::string& path, std::string& error) const
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        error = "unable to create sidecar: " + path;
        return false;
    }

    const std::uint32_t magic = kMagic;
    const std::uint32_t version = kVersion;
    if (!WritePod(out, magic) || !WritePod(out, version) ||
        !WritePod(out, sourceSize) || !WritePod(out, sourceMtime) ||
        !WriteString(out, timescale) ||
        !WritePod(out, maxTime) || !WritePod(out, dataOffset)) {
        error = "sidecar write failed: " + path;
        return false;
    }

    const std::uint32_t signalCount = static_cast<std::uint32_t>(signals.size());
    if (!WritePod(out, signalCount)) {
        error = "sidecar write failed: " + path;
        return false;
    }
    for (const SignalEntry& entry : signals) {
        const std::uint8_t kind = static_cast<std::uint8_t>(entry.kind);
        if (!WriteString(out, entry.idCode) || !WriteString(out, entry.name) ||
            !WriteString(out, entry.scope) || !WritePod(out, entry.width) ||
            !WritePod(out, kind)) {
            error = "sidecar write failed: " + path;
            return false;
        }
    }

    const std::uint64_t sampleCount = static_cast<std::uint64_t>(samples.size());
    if (!WritePod(out, sampleCount)) {
        error = "sidecar write failed: " + path;
        return false;
    }
    for (const TimeSample& sample : samples) {
        if (!WritePod(out, sample.time) || !WritePod(out, sample.offset)) {
            error = "sidecar write failed: " + path;
            return false;
        }
    }

    out.flush();
    if (!out) {
        error = "sidecar write failed: " + path;
        return false;
    }
    return true;
}

bool TraceSidecarIndex::Load(const std::string& path, std::string& error)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "unable to open sidecar: " + path;
        return false;
    }

    std::uint32_t magic = 0;
    std::uint32_t version = 0;
    if (!ReadPod(in, magic) || !ReadPod(in, version)) {
        error = "truncated sidecar: " + path;
        return false;
    }
    if (magic != kMagic || version != kVersion) {
        error = "unsupported sidecar version: " + path;
        return false;
    }

    if (!ReadPod(in, sourceSize) || !ReadPod(in, sourceMtime) ||
        !ReadString(in, timescale) ||
        !ReadPod(in, maxTime) || !ReadPod(in, dataOffset)) {
        error = "truncated sidecar: " + path;
        return false;
    }

    std::uint32_t signalCount = 0;
    if (!ReadPod(in, signalCount) || signalCount > (1u << 20)) {
        error = "corrupt sidecar: " + path;
        return false;
    }
    signals.clear();
    signals.reserve(signalCount);
    for (std::uint32_t i = 0; i < signalCount; ++i) {
        SignalEntry entry;
        std::uint8_t kind = 0;
        if (!ReadString(in, entry.idCode) || !ReadString(in, entry.name) ||
            !ReadString(in, entry.scope) || !ReadPod(in, entry.width) ||
            !ReadPod(in, kind)) {
            error = "corrupt sidecar: " + path;
            return false;
        }
        if (kind > static_cast<std::uint8_t>(SignalKind::String)) {
            error = "corrupt sidecar: " + path;
            return false;
        }
        entry.kind = static_cast<SignalKind>(kind);
        signals.push_back(std::move(entry));
    }

    std::uint64_t sampleCount = 0;
    if (!ReadPod(in, sampleCount) || sampleCount > (1ull << 26)) {
        error = "corrupt sidecar: " + path;
        return false;
    }
    samples.clear();
    samples.reserve(static_cast<std::size_t>(sampleCount));
    for (std::uint64_t i = 0; i < sampleCount; ++i) {
        TimeSample sample;
        if (!ReadPod(in, sample.time) || !ReadPod(in, sample.offset)) {
            error = "corrupt sidecar: " + path;
            return false;
        }
        samples.push_back(sample);
    }
    return true;
}

} // namespace trace
} // namespace sigflow
