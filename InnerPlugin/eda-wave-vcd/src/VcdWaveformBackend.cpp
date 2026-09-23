#include "VcdWaveformBackend.h"

#include <cstdint>
#include <fstream>
#include <sstream>

namespace eda {
namespace wave {
namespace {

std::string Trim(const std::string& value) {
    const std::size_t first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const std::size_t last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::vector<std::string> SplitTokens(const std::string& value) {
    std::vector<std::string> tokens;
    std::istringstream stream(value);
    std::string token;
    while (stream >> token) tokens.push_back(token);
    return tokens;
}

std::string JoinScope(const std::vector<std::string>& scope) {
    std::string result;
    for (const auto& part : scope) {
        if (!result.empty()) result += "/";
        result += part;
    }
    return result;
}

} // namespace

bool VcdWaveformBackend::Open(const std::string& path, std::string& error) {
    signals_.clear();
    changes_.clear();
    idToIndex_.clear();
    timescale_.clear();
    timeRange_ = WaveTimeRange();

    std::ifstream input(path, std::ios::binary);
    if (!input) {
        error = "VCD file not found: " + path;
        return false;
    }

    std::vector<std::string> scopeStack;
    bool inDefinitions = false;
    std::uint64_t currentTime = 0;
    bool sawTime = false;

    std::string line;
    while (std::getline(input, line)) {
        line = Trim(line);
        if (line.empty()) continue;

        if (line.rfind("$scope", 0) == 0) {
            const auto tokens = SplitTokens(line);
            if (tokens.size() >= 3) scopeStack.push_back(tokens[2]);
            continue;
        }
        if (line.rfind("$upscope", 0) == 0) {
            if (!scopeStack.empty()) scopeStack.pop_back();
            continue;
        }
        if (line.rfind("$var", 0) == 0) {
            const auto tokens = SplitTokens(line);
            if (tokens.size() >= 5) {
                WaveSignal signal;
                signal.id = static_cast<int>(signals_.size());
                signal.width = static_cast<unsigned>(std::stoul(tokens[2]));
                signal.idCode = tokens[3];
                signal.name = tokens[4];
                signal.scope = JoinScope(scopeStack);
                signal.fullName = signal.scope.empty() ? signal.name : signal.scope + "." + signal.name;
                idToIndex_[signal.idCode] = signal.id;
                signals_.push_back(signal);
                changes_.emplace_back();
            }
            continue;
        }
        if (line.rfind("$timescale", 0) == 0) {
            const auto tokens = SplitTokens(line);
            if (tokens.size() >= 2) {
                timescale_ = tokens[1];
                if (timescale_.rfind("$end", 0) == 0) timescale_.clear();
            }
            continue;
        }
        if (line.rfind("$enddefinitions", 0) == 0) {
            inDefinitions = true;
            continue;
        }
        if (line[0] == '$') continue;  // 其他指令（$date/$version/$dumpvars/$end 等）

        if (!inDefinitions) continue;

        if (line[0] == '#') {
            try {
                currentTime = std::stoull(line.substr(1));
                sawTime = true;
            } catch (const std::exception&) {
                // 忽略非法时间标记
            }
            continue;
        }

        // 值变化：b<bits> <id> 或 <scalar><id>
        std::string value;
        std::string idCode;
        if (line[0] == 'b' || line[0] == 'B') {
            const std::size_t space = line.find(' ');
            if (space == std::string::npos) continue;
            value = line.substr(1, space - 1);
            idCode = Trim(line.substr(space + 1));
        } else {
            value = line.substr(0, 1);
            idCode = Trim(line.substr(1));
        }
        const auto found = idToIndex_.find(idCode);
        if (found == idToIndex_.end()) continue;
        changes_[found->second].push_back({currentTime, value});
        if (!timeRange_.valid || currentTime > timeRange_.end) timeRange_.end = currentTime;
        timeRange_.valid = true;
    }

    if (!sawTime && timeRange_.valid) timeRange_.end = 0;
    if (signals_.empty()) {
        error = "VCD file contains no signals: " + path;
        return false;
    }
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
