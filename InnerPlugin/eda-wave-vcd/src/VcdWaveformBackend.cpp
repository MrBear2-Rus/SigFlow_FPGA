#include "VcdWaveformBackend.h"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <fstream>
#include <limits>

namespace eda {
namespace wave {
namespace {

template <typename UInt>
bool ParseUnsigned(const std::string& token, UInt& value) {
    if (token.empty() || token.front() < '0' || token.front() > '9') return false;
    const auto parsed = std::from_chars(token.data(), token.data() + token.size(), value);
    return parsed.ec == std::errc() && parsed.ptr == token.data() + token.size();
}

std::string JoinScope(const std::vector<std::string>& scope) {
    std::string result;
    for (const auto& part : scope) {
        if (!result.empty()) result += "/";
        result += part;
    }
    return result;
}

bool ReadDirective(std::istream& input, std::vector<std::string>& arguments) {
    arguments.clear();
    std::string token;
    while (input >> token) {
        if (token == "$end") return true;
        arguments.push_back(token);
    }
    return false;
}

bool DigitalBit(char bit) {
    return bit == '0' || bit == '1' || bit == 'x' || bit == 'X' || bit == 'z' || bit == 'Z';
}

} // namespace

bool VcdWaveformBackend::Open(const std::string& path, std::string& error) {
    const auto clear = [&] {
        signals_.clear();
        changes_.clear();
        idToIndices_.clear();
        timescale_.clear();
        timeRange_ = WaveTimeRange();
    };
    clear();
    error.clear();
    const auto fail = [&](const std::string& detail) {
        clear(); // A failed load must not expose an apparently usable partial trace.
        error = "Invalid or unsupported VCD: " + detail;
        return false;
    };
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        error = "VCD file not found: " + path;
        return false;
    }

    std::vector<std::string> scopeStack;
    bool inDefinitions = true;
    bool inDump = false;
    constexpr unsigned kMaxWidth = 4096;
    constexpr std::size_t kMaxSignals = 65536;
    constexpr std::size_t kMaxExpandedBytes = 64 * 1024 * 1024;
    std::size_t expandedBytes = 0;
    std::uint64_t currentTime = 0;
    std::string token;
    while (input >> token) {
        if (token == "$comment" || token == "$date" || token == "$version") {
            std::vector<std::string> ignored;
            if (!ReadDirective(input, ignored)) return fail("unterminated " + token);
            continue;
        }
        if (token.front() == '$') {
            if (!inDefinitions) {
                if (token == "$dumpvars" || token == "$dumpall" ||
                    token == "$dumpon" || token == "$dumpoff") {
                    if (inDump) return fail("nested dump directive");
                    inDump = true;
                    continue;
                }
                if (token == "$end" && inDump) {
                    inDump = false;
                    continue;
                }
                return fail("unexpected data directive " + token);
            }

            std::vector<std::string> arguments;
            if (!ReadDirective(input, arguments)) return fail("unterminated " + token);
            if (token == "$scope") {
                if (arguments.size() != 2) return fail("malformed scope");
                scopeStack.push_back(arguments[1]);
            } else if (token == "$upscope") {
                if (!arguments.empty() || scopeStack.empty()) return fail("unmatched upscope");
                scopeStack.pop_back();
            } else if (token == "$var") {
                if (arguments.size() < 4 || arguments.size() > 5) return fail("malformed variable");
                if (arguments[0] == "real" || arguments[0] == "realtime" || arguments[0] == "string")
                    return fail("non-digital variable type " + arguments[0]);
                unsigned width = 0;
                if (!ParseUnsigned(arguments[1], width) || width == 0 || width > kMaxWidth)
                    return fail("invalid or unsupported variable width");
                if (signals_.size() >= kMaxSignals)
                    return fail("too many signals");
                const auto found = idToIndices_.find(arguments[2]);
                if (found != idToIndices_.end() && signals_[found->second.front()].width != width)
                    return fail("inconsistent alias widths for " + arguments[2]);
                WaveSignal signal;
                signal.id = static_cast<int>(signals_.size());
                signal.width = width;
                signal.idCode = arguments[2];
                signal.name = arguments[3];
                signal.scope = JoinScope(scopeStack);
                signal.fullName = signal.scope.empty() ? signal.name : signal.scope + "." + signal.name;
                idToIndices_[signal.idCode].push_back(signal.id);
                signals_.push_back(signal);
                changes_.emplace_back();
            } else if (token == "$timescale") {
                if (arguments.empty() || arguments.size() > 2) return fail("malformed timescale");
                timescale_ = arguments[0];
                if (arguments.size() == 2) timescale_ += arguments[1];
            } else if (token == "$enddefinitions") {
                if (!arguments.empty() || !scopeStack.empty()) return fail("malformed enddefinitions");
                inDefinitions = false;
            } else {
                return fail("unexpected definition directive " + token);
            }
            continue;
        }
        if (inDefinitions) return fail("value before enddefinitions");
        if (token.front() == '#') {
            std::uint64_t nextTime = 0;
            if (!ParseUnsigned(token.substr(1), nextTime) || nextTime < currentTime)
                return fail("invalid or decreasing timestamp " + token);
            currentTime = nextTime;
            timeRange_.valid = true;
            timeRange_.end = currentTime;
            continue;
        }

        std::string value;
        std::string idCode;
        const bool vector = token.front() == 'b' || token.front() == 'B';
        if (vector) {
            value = token.substr(1);
            if (!(input >> idCode)) return fail("missing vector identifier");
        } else {
            value = token.substr(0, 1);
            idCode = token.substr(1);
        }
        if (value.empty() || !std::all_of(value.begin(), value.end(), DigitalBit))
            return fail("invalid or unsupported digital value");
        const auto found = idToIndices_.find(idCode);
        if (found == idToIndices_.end()) return fail("undeclared value identifier " + idCode);
        const unsigned width = signals_[found->second.front()].width;
        if ((!vector && width != 1) || value.size() > width) return fail("value exceeds declared width");
        // Account for every alias copy and its transition object before expanding
        // compressed vectors. A tiny VCD must not allocate multi-GiB strings.
        const std::size_t bytesPerAlias = width + sizeof(WaveTransition);
        if (found->second.size() > (kMaxExpandedBytes-expandedBytes)/bytesPerAlias)
            return fail("expanded event budget exceeded");
        expandedBytes += found->second.size()*bytesPerAlias;
        for (auto& bit : value) {
            if (bit == 'X') bit = 'x';
            if (bit == 'Z') bit = 'z';
        }
        // VCD permits omitted leading zeroes, or leading x/z extension bits.
        const char fill = value.front() == 'x' || value.front() == 'z' ? value.front() : '0';
        value.insert(0, width - value.size(), fill);
        for (int index : found->second) changes_[index].push_back({currentTime, value});
        timeRange_.valid = true;
        timeRange_.end = currentTime;
    }
    if (input.bad()) return fail("file read failed");
    if (inDefinitions || inDump) return fail("unterminated definitions or dump directive");
    if (signals_.empty()) return fail("file contains no signals");
    return true;
}

bool VcdWaveformBackend::Query(int signalId, std::uint64_t t0, std::uint64_t t1,
                               std::vector<WaveTransition>& out, std::string& error) {
    out.clear();
    if (signalId < 0 || signalId >= static_cast<int>(changes_.size())) {
        error = "unknown signal id";
        return false;
    }
    for (const auto& transition : changes_[signalId]) {
        if (transition.time >= t0 && transition.time <= t1) out.push_back(transition);
    }
    return true;
}

bool VcdWaveformBackend::ValueAt(int signalId, std::uint64_t t, std::string& value,
                                 std::string& error) {
    value.clear();
    if (signalId < 0 || signalId >= static_cast<int>(changes_.size())) {
        error = "unknown signal id";
        return false;
    }
    for (const auto& transition : changes_[signalId]) {
        if (transition.time > t) break;
        value = transition.value;
    }
    return true;
}

} // namespace wave
} // namespace eda
