#include "WaveProtocolDecoder.h"

#include "../trace/TraceTimebase.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <sstream>

namespace sigflow::wave {
namespace {

using sigflow::trace::SignalInfo;
using sigflow::trace::TimeValue;
using sigflow::trace::TraceSource;
using sigflow::trace::Transition;

bool Prepare(TraceSource& source, const ProtocolDecodeOptions& options,
             TimeValue& start, TimeValue& end, std::uint64_t& tickFs,
             std::string& error)
{
    if (options.requirePhysicalTime &&
        source.TimeDomain() != sigflow::trace::TraceTimeDomain::Physical) {
        error = "TIME_DOMAIN_UNKNOWN: protocol decoding requires physical VCD time";
        return false;
    }
    if (!sigflow::trace::VcdTickFemtoseconds(source.Timescale(), tickFs)) {
        error = "TIME_DOMAIN_UNKNOWN: invalid or missing VCD timescale";
        return false;
    }
    start = options.startTime;
    end = options.endTime == 0 ? source.TimeRange().end : options.endTime;
    if (!source.TimeRange().valid || end < start) {
        error = "invalid protocol decode time range";
        return false;
    }
    sigflow::trace::TraceCoverageRange gap;
    if (source.Coverage().FindGap(start, end, gap)) {
        std::ostringstream message;
        message << "TRACE_GAP: protocol decode range crosses uncovered trace interval ["
                << gap.begin << "," << gap.end << "]";
        if (!gap.reason.empty()) message << " (" << gap.reason << ")";
        error = message.str();
        return false;
    }
    return true;
}

const SignalInfo* Signal(TraceSource& source, int id, const char* role,
                        std::string& error)
{
    const SignalInfo* signal = source.SignalById(id);
    if (!signal) error = std::string("missing ") + role + " signal";
    return signal;
}

bool BitAt(TraceSource& source, const SignalInfo& signal, TimeValue time,
           bool& bit, std::string& error)
{
    std::string value;
    if (!source.ValueAt(signal, time, value, error)) return false;
    if (value == "0" || value == "1") {
        bit = value == "1";
        return true;
    }
    error = "non-binary value on signal " + signal.fullName;
    return false;
}

bool BinaryAt(TraceSource& source, const SignalInfo& signal, TimeValue time,
              std::uint64_t& value, std::string& error)
{
    std::string bits;
    if (!source.ValueAt(signal, time, bits, error)) return false;
    if (bits.empty() || bits.size() > 64) {
        error = "unsupported data width on signal " + signal.fullName;
        return false;
    }
    value = 0;
    for (const char bit : bits) {
        if (bit != '0' && bit != '1') {
            error = "non-binary data value on signal " + signal.fullName;
            return false;
        }
        value = (value << 1) | static_cast<std::uint64_t>(bit - '0');
    }
    return true;
}

bool QueryInChunks(TraceSource& source, const SignalInfo& signal,
                   TimeValue start, TimeValue end, TimeValue chunkSize,
                   const ProtocolDecodeOptions& options,
                   std::vector<Transition>& out, std::string& error)
{
    out.clear();
    if (options.shouldCancel && options.shouldCancel()) {
        error = "DECODE_CANCELLED: protocol decode was cancelled";
        return false;
    }
    if (chunkSize == 0) return source.Query(signal, start, end, out, error);
    TimeValue cursor = start;
    while (true) {
        if (options.shouldCancel && options.shouldCancel()) {
            error = "DECODE_CANCELLED: protocol decode was cancelled";
            return false;
        }
        const TimeValue remaining = std::numeric_limits<TimeValue>::max() - cursor;
        const TimeValue span = std::min(chunkSize, remaining);
        const TimeValue chunkEnd = std::min(end, cursor + span);
        std::vector<Transition> chunk;
        if (!source.Query(signal, cursor, chunkEnd, chunk, error)) return false;
        for (const Transition& transition : chunk) {
            if (options.shouldCancel && options.shouldCancel()) {
                error = "DECODE_CANCELLED: protocol decode was cancelled";
                return false;
            }
            if (!out.empty() && out.back().time == transition.time &&
                out.back().value == transition.value) {
                continue;
            }
            out.push_back(transition);
        }
        if (chunkEnd == end) break;
        if (chunkEnd == std::numeric_limits<TimeValue>::max()) {
            error = "protocol query chunk range overflow";
            return false;
        }
        cursor = chunkEnd + 1;
    }
    return true;
}

bool CheckCancelled(const ProtocolDecodeOptions& options, std::string& error)
{
    if (options.shouldCancel && options.shouldCancel()) {
        error = "DECODE_CANCELLED: protocol decode was cancelled";
        return true;
    }
    return false;
}

bool AddOffset(TimeValue origin, long double ticks, TimeValue end,
               TimeValue& result)
{
    if (ticks < 0 || ticks > static_cast<long double>(std::numeric_limits<TimeValue>::max())) return false;
    const auto delta = static_cast<TimeValue>(std::floor(ticks + 0.5L));
    if (origin > std::numeric_limits<TimeValue>::max() - delta) return false;
    result = origin + delta;
    return result <= end;
}

bool IsParityGood(std::uint64_t value, unsigned bits, bool parityBit,
                  UartDecodeConfig::Parity parity)
{
    if (parity == UartDecodeConfig::Parity::None) return true;
    bool ones = parityBit;
    for (unsigned i = 0; i < bits; ++i) ones ^= ((value >> i) & 1u) != 0;
    return parity == UartDecodeConfig::Parity::Even ? !ones : ones;
}

std::string HexByte(std::uint8_t value)
{
    const char* digits = "0123456789ABCDEF";
    std::string text = "0x00";
    text[2] = digits[value >> 4];
    text[3] = digits[value & 15];
    return text;
}

} // namespace

bool DecodeUart(TraceSource& source, const UartDecodeConfig& config,
                std::vector<ProtocolTransaction>& out, std::string& error)
{
    out.clear();
    if (config.baudRate == 0 || config.dataBits < 5 || config.dataBits > 9 ||
        (config.stopBits != 1 && config.stopBits != 2)) {
        error = "UART requires baud_rate, data bits 5..9 and one or two stop bits";
        return false;
    }
    TimeValue start = 0, end = 0;
    std::uint64_t tickFs = 0;
    if (!Prepare(source, config.options, start, end, tickFs, error)) return false;
    const SignalInfo* signal = Signal(source, config.signalId, "UART", error);
    if (!signal) return false;
    bool idle = false;
    if (!BitAt(source, *signal, start, idle, error)) return false;
    if (config.invert) idle = !idle;
    std::vector<Transition> transitions;
    if (!QueryInChunks(source, *signal, start, end, config.options.queryChunkSize,
                       config.options,
                       transitions, error)) return false;
    const long double bitTicks = 1000000000000000.0L /
        (static_cast<long double>(config.baudRate) * static_cast<long double>(tickFs));
    if (bitTicks < 1.0L) {
        error = "UART bit period is below one VCD tick";
        return false;
    }
    bool previous = idle;
    TimeValue nextAllowed = start;
    for (const Transition& transition : transitions) {
        if (CheckCancelled(config.options, error)) return false;
        bool current = transition.value == "1";
        if (config.invert) current = !current;
        if (previous == current) continue;
        if (previous && !current && transition.time >= nextAllowed) {
            ProtocolTransaction event;
            event.protocol = "UART";
            event.startTime = transition.time;
            std::uint64_t data = 0;
            bool valid = true;
            bool truncated = false;
            std::ostringstream detail;
            for (unsigned bit = 0; bit < config.dataBits; ++bit) {
                if (CheckCancelled(config.options, error)) return false;
                TimeValue sample = 0;
                if (!AddOffset(transition.time, (1.5L + bit) * bitTicks, end, sample)) {
                    valid = false;
                    truncated = true;
                    break;
                }
                bool value = false;
                if (!BitAt(source, *signal, sample, value, error)) return false;
                if (config.invert) value = !value;
                if (value) data |= std::uint64_t(1) << bit;
            }
            bool parityBit = false;
            if (valid && config.parity != UartDecodeConfig::Parity::None) {
                TimeValue sample = 0;
                if (!AddOffset(transition.time, (1.5L + config.dataBits) * bitTicks, end, sample)) {
                    valid = false;
                    truncated = true;
                } else if (!BitAt(source, *signal, sample, parityBit, error)) return false;
                if (!valid) {
                    // The parity sample is outside the requested window; keep
                    // the frame as evidence instead of turning it into a query error.
                } else {
                if (config.invert) parityBit = !parityBit;
                valid = IsParityGood(data, config.dataBits, parityBit, config.parity);
                if (!valid) detail << "parity error; ";
                }
            }
            const unsigned stopStart = config.parity == UartDecodeConfig::Parity::None
                ? config.dataBits : config.dataBits + 1;
            for (unsigned stop = 0; valid && stop < config.stopBits; ++stop) {
                TimeValue sample = 0;
                if (!AddOffset(transition.time, (1.5L + stopStart + stop) * bitTicks, end, sample)) {
                    valid = false;
                    truncated = true;
                    break;
                }
                bool value = false;
                if (!BitAt(source, *signal, sample, value, error)) return false;
                if (config.invert) value = !value;
                if (!value) { valid = false; detail << "stop bit error; "; }
                event.endTime = sample;
            }
            if (event.endTime == 0) event.endTime = truncated ? end : transition.time;
            event.valid = valid;
            event.words.push_back(data);
            if (config.dataBits <= 8) event.bytes.push_back(static_cast<std::uint8_t>(data));
            if (truncated) detail << "truncated frame; ";
            detail << (valid ? "byte " + HexByte(static_cast<std::uint8_t>(data)) : "invalid frame");
            event.detail = detail.str();
            if (valid || config.options.includeInvalid) out.push_back(std::move(event));
            nextAllowed = event.endTime + 1;
        }
        previous = current;
    }
    error.clear();
    return true;
}

bool DecodeSpi(TraceSource& source, const SpiDecodeConfig& config,
               std::vector<ProtocolTransaction>& out, std::string& error)
{
    out.clear();
    if (config.bitsPerWord == 0 || config.bitsPerWord > 64) {
        error = "SPI word width must be in 1..64";
        return false;
    }
    if (config.mosiSignalId < 0 && config.misoSignalId < 0) {
        error = "SPI requires MOSI or MISO";
        return false;
    }
    TimeValue start = 0, end = 0;
    std::uint64_t tickFs = 0;
    if (!Prepare(source, config.options, start, end, tickFs, error)) return false;
    const SignalInfo* clock = Signal(source, config.clockSignalId, "SPI clock", error);
    const SignalInfo* mosi = config.mosiSignalId < 0 ? nullptr : Signal(source, config.mosiSignalId, "SPI MOSI", error);
    const SignalInfo* miso = config.misoSignalId < 0 ? nullptr : Signal(source, config.misoSignalId, "SPI MISO", error);
    const SignalInfo* cs = config.chipSelectSignalId < 0 ? nullptr : Signal(source, config.chipSelectSignalId, "SPI chip select", error);
    if (!clock || (config.mosiSignalId >= 0 && !mosi) || (config.misoSignalId >= 0 && !miso) ||
        (config.chipSelectSignalId >= 0 && !cs)) return false;
    bool previousClock = false;
    if (!BitAt(source, *clock, start, previousClock, error)) return false;
    std::vector<Transition> transitions;
    if (!QueryInChunks(source, *clock, start, end, config.options.queryChunkSize,
                       config.options,
                       transitions, error)) return false;
    const bool leadingLevel = !config.clockPolarity;
    bool active = cs == nullptr;
    std::uint64_t mosiWord = 0, misoWord = 0;
    unsigned bitCount = 0;
    TimeValue wordStart = 0;
    auto flush = [&](TimeValue time, bool complete) {
        if (bitCount == 0) return;
        ProtocolTransaction event;
        event.protocol = "SPI";
        event.startTime = wordStart;
        event.endTime = time;
        event.valid = complete;
        if (mosi) event.words.push_back(mosiWord);
        if (miso) event.words.push_back(misoWord);
        if (mosi && config.bitsPerWord <= 8) event.bytes.push_back(static_cast<std::uint8_t>(mosiWord));
        event.detail = complete ? "word" : "partial word (chip-select/window ended)";
        if (event.valid || config.options.includeInvalid) out.push_back(std::move(event));
        bitCount = 0; mosiWord = 0; misoWord = 0;
    };
    for (const Transition& transition : transitions) {
        if (CheckCancelled(config.options, error)) return false;
        bool currentClock = transition.value == "1";
        const bool leading = previousClock != currentClock && currentClock == leadingLevel;
        const bool sampleEdge = config.clockPhase ? !leading : leading;
        bool chipActive = true;
        if (cs) {
            if (!BitAt(source, *cs, transition.time, chipActive, error)) return false;
            if (config.chipSelectActiveLow) chipActive = !chipActive;
        }
        if (!chipActive && active) flush(transition.time, false);
        active = chipActive;
        if (active && sampleEdge) {
            bool mosiBit = false, misoBit = false;
            if (mosi && !BitAt(source, *mosi, transition.time, mosiBit, error)) return false;
            if (miso && !BitAt(source, *miso, transition.time, misoBit, error)) return false;
            if (bitCount == 0) wordStart = transition.time;
            const unsigned shift = config.lsbFirst ? bitCount : (config.bitsPerWord - bitCount - 1);
            if (mosi && mosiBit) mosiWord |= std::uint64_t(1) << shift;
            if (miso && misoBit) misoWord |= std::uint64_t(1) << shift;
            ++bitCount;
            if (bitCount == config.bitsPerWord) flush(transition.time, true);
        }
        previousClock = currentClock;
    }
    if (active) flush(end, false);
    error.clear();
    return true;
}

bool DecodeValidReady(TraceSource& source, const ValidReadyDecodeConfig& config,
                      std::vector<ProtocolTransaction>& out, std::string& error)
{
    out.clear();
    TimeValue start = 0, end = 0;
    std::uint64_t tickFs = 0;
    if (!Prepare(source, config.options, start, end, tickFs, error)) return false;
    const SignalInfo* clock = Signal(source, config.clockSignalId, "valid-ready clock", error);
    const SignalInfo* valid = Signal(source, config.validSignalId, "valid", error);
    const SignalInfo* ready = Signal(source, config.readySignalId, "ready", error);
    const SignalInfo* data = Signal(source, config.dataSignalId, "valid-ready data", error);
    if (!clock || !valid || !ready || !data) return false;
    if (config.dataWidth > 64 || config.dataBitOffset > data->width ||
        (config.dataWidth != 0 && config.dataWidth > data->width - config.dataBitOffset)) {
        error = "valid-ready data bit range exceeds the signal width";
        return false;
    }
    const unsigned dataWidth = config.dataWidth == 0 ? data->width : config.dataWidth;
    if (dataWidth == 0 || dataWidth > 64) {
        error = "valid-ready data width must be in 1..64";
        return false;
    }
    bool previousClock = false;
    if (!BitAt(source, *clock, start, previousClock, error)) return false;
    std::vector<Transition> transitions;
    if (!QueryInChunks(source, *clock, start, end, config.options.queryChunkSize,
                       config.options,
                       transitions, error)) return false;
    TimeValue waitStart = 0;
    std::uint64_t waitCycles = 0;
    for (const Transition& transition : transitions) {
        if (CheckCancelled(config.options, error)) return false;
        const bool currentClock = transition.value == "1";
        const bool edge = config.risingEdge ? (!previousClock && currentClock) : (previousClock && !currentClock);
        previousClock = currentClock;
        if (!edge) continue;
        bool v = false, r = false;
        if (!BitAt(source, *valid, transition.time, v, error) || !BitAt(source, *ready, transition.time, r, error)) return false;
        if (v && !r) {
            if (waitCycles == 0) waitStart = transition.time;
            ++waitCycles;
            continue;
        }
        if (!v) {
            if (waitCycles != 0 && config.options.includeInvalid) {
                out.push_back({"valid-ready", waitStart, transition.time, false, {}, {}, waitCycles,
                               "valid dropped before ready"});
            }
            waitCycles = 0;
            continue;
        }
        if (r) {
            std::uint64_t value = 0;
            if (!BinaryAt(source, *data, transition.time, value, error)) return false;
            if (config.dataBitOffset != 0 || dataWidth < data->width) {
                value >>= config.dataBitOffset;
                if (dataWidth < 64) value &= (std::uint64_t(1) << dataWidth) - 1;
            }
            const TimeValue begin = waitCycles == 0 ? transition.time : waitStart;
            ProtocolTransaction event;
            event.protocol = "valid-ready";
            event.startTime = begin;
            event.endTime = transition.time;
            event.valid = true;
            event.words.push_back(value);
            event.waitCycles = waitCycles;
            event.detail = waitCycles == 0 ? "transfer" : "transfer after backpressure";
            out.push_back(std::move(event));
            waitCycles = 0;
        }
    }
    if (waitCycles != 0 && config.options.includeInvalid)
        out.push_back({"valid-ready", waitStart, end, false, {}, {}, waitCycles,
                       "window ended while valid was waiting"});
    error.clear();
    return true;
}

} // namespace sigflow::wave
