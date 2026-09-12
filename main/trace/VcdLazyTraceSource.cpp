#include "VcdLazyTraceSource.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

namespace sigflow {
namespace trace {

namespace {

std::int64_t FileMtime(const std::string& path)
{
    std::error_code ec;
    const auto fileTime = std::filesystem::last_write_time(path, ec);
    if (ec) return 0;
    const auto systemTime = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        fileTime - std::filesystem::file_time_type::clock::now() +
        std::chrono::system_clock::now());
    return std::chrono::system_clock::to_time_t(systemTime);
}

} // namespace

void VcdLazyTraceSource::TrimLine(std::string& line)
{
    const std::size_t first = line.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        line.clear();
        return;
    }
    if (first > 0) line.erase(0, first);
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n' ||
                             line.back() == ' ' || line.back() == '\t')) {
        line.pop_back();
    }
}

TimeValue VcdLazyTraceSource::ParseTime(const std::string& text)
{
    const char* p = text.c_str();
    while (*p == ' ' || *p == '\t' || *p == '+') ++p;
    TimeValue value = 0;
    while (*p >= '0' && *p <= '9') {
        value = value * 10 + static_cast<TimeValue>(*p - '0');
        ++p;
    }
    return value;
}

bool VcdLazyTraceSource::ScanHeader(std::ifstream& in, std::string& error)
{
    std::vector<std::string> scopeStack;
    int nextId = 0;
    std::string line;
    while (std::getline(in, line)) {
        TrimLine(line);
        if (line.empty()) continue;

        if (line.rfind("$enddefinitions", 0) == 0) {
            const auto pos = in.tellg();
            m_dataOffset = (pos == static_cast<std::streamoff>(-1))
                               ? 0
                               : static_cast<std::uint64_t>(pos);
            return true;
        }

        if (line.rfind("$timescale", 0) == 0) {
            std::istringstream stream(line);
            std::string token;
            std::string unit;
            stream >> token >> unit;
            if (!unit.empty()) m_timescale = unit;
        } else if (line.rfind("$scope", 0) == 0) {
            std::istringstream stream(line);
            std::string token;
            std::string kind;
            std::string name;
            stream >> token >> kind >> name;
            if (!name.empty()) scopeStack.push_back(name);
        } else if (line.rfind("$upscope", 0) == 0) {
            if (!scopeStack.empty()) scopeStack.pop_back();
        } else if (line.rfind("$var", 0) == 0) {
            std::string declaration = line;
            while (declaration.find("$end") == std::string::npos) {
                std::string continuation;
                if (!std::getline(in, continuation)) {
                    error = "unterminated $var declaration: " + m_path;
                    return false;
                }
                TrimLine(continuation);
                if (!continuation.empty()) declaration += " " + continuation;
            }

            std::istringstream stream(declaration);
            std::string token;
            std::string kind;
            std::string idCode;
            std::string name;
            unsigned width = 1;
            stream >> token >> kind >> width >> idCode >> name;
            if (idCode.empty() || name.empty()) continue;

            SignalInfo info;
            info.id = nextId++;
            info.idCode = idCode;
            info.name = name;
            for (const std::string& scope : scopeStack) {
                if (!info.scope.empty()) info.scope += "/";
                info.scope += scope;
            }
            info.fullName = info.scope.empty() ? name : info.scope + "." + name;
            info.width = width;
            if (kind == "real") {
                info.kind = SignalKind::Real;
            } else if (kind == "string") {
                info.kind = SignalKind::String;
            } else {
                info.kind = (width == 1) ? SignalKind::Scalar : SignalKind::Vector;
            }
            m_idToSignal[idCode] = info.id;
            m_idToIndex[info.id] = static_cast<int>(m_signals.size());
            m_signals.push_back(std::move(info));
        }
        // $date / $version / $comment / $dumpvars 等在头部阶段直接跳过
    }
    error = "VCD header missing $enddefinitions: " + m_path;
    return false;
}

bool VcdLazyTraceSource::BuildTimeIndex(std::ifstream& in)
{
    in.clear();
    in.seekg(static_cast<std::streamoff>(m_dataOffset));
    m_timeIndex.clear();
    m_timeRange = TraceTimeRange{};

    TimeValue currentTime = 0;
    bool first = true;
    std::uint64_t lastSampleOffset = m_dataOffset;
    std::string line;
    while (true) {
        const auto pos = in.tellg();
        if (pos == static_cast<std::streamoff>(-1)) break;
        if (!std::getline(in, line)) break;
        const std::uint64_t lineStart = static_cast<std::uint64_t>(pos);
        TrimLine(line);
        if (line.empty()) continue;
        if (line[0] == '#') {
            currentTime = ParseTime(line.substr(1));
            if (currentTime > m_timeRange.end) m_timeRange.end = currentTime;
            if (first || (lineStart - lastSampleOffset) >= m_sampleStrideBytes) {
                m_timeIndex.push_back({currentTime, lineStart});
                lastSampleOffset = lineStart;
                first = false;
            }
        }
    }
    m_timeRange.begin = 0;
    m_timeRange.valid = true;
    const auto endPos = in.tellg();
    m_indexScanBytes = (endPos == static_cast<std::streamoff>(-1))
                           ? m_fileSize
                           : static_cast<std::uint64_t>(endPos) - m_dataOffset;
    return true;
}

bool VcdLazyTraceSource::Open(const std::string& path, std::string& error)
{
    m_path = path;
    m_signals.clear();
    m_idToSignal.clear();
    m_idToIndex.clear();
    m_timeIndex.clear();
    m_timeRange = TraceTimeRange{};
    m_sidecarLoaded = false;
    m_indexScanBytes = 0;

    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec) {
        error = "unable to stat " + path;
        return false;
    }
    m_fileSize = size;
    m_fileMtime = FileMtime(path);

    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "unable to open " + path;
        return false;
    }
    if (!ScanHeader(in, error)) return false;
    if (m_signals.empty()) {
        error = "VCD declares no signals: " + path;
        return false;
    }

    const std::string sidecarPath = TraceSidecarIndex::SidecarPathFor(path);
    TraceSidecarIndex sidecar;
    std::string sidecarError;
    if (sidecar.Load(sidecarPath, sidecarError) &&
        sidecar.sourceSize == m_fileSize &&
        sidecar.sourceMtime == m_fileMtime &&
        sidecar.signals.size() == m_signals.size()) {
        m_timeIndex = std::move(sidecar.samples);
        m_timeRange.begin = 0;
        m_timeRange.end = sidecar.maxTime;
        m_timeRange.valid = true;
        if (!sidecar.timescale.empty()) m_timescale = sidecar.timescale;
        m_sidecarLoaded = true;
        return true;
    }

    if (!BuildTimeIndex(in)) {
        error = "unable to build time index: " + path;
        return false;
    }

    TraceSidecarIndex toSave;
    toSave.sourcePath = path;
    toSave.sourceSize = m_fileSize;
    toSave.sourceMtime = m_fileMtime;
    toSave.timescale = m_timescale;
    toSave.maxTime = m_timeRange.end;
    toSave.dataOffset = m_dataOffset;
    for (const SignalInfo& signal : m_signals) {
        TraceSidecarIndex::SignalEntry entry;
        entry.idCode = signal.idCode;
        entry.name = signal.name;
        entry.scope = signal.scope;
        entry.width = signal.width;
        entry.kind = signal.kind;
        toSave.signals.push_back(std::move(entry));
    }
    toSave.samples = m_timeIndex;
    std::string saveError;
    if (toSave.Save(sidecarPath, saveError)) m_sidecarLoaded = true;
    return true;
}

const SignalInfo* VcdLazyTraceSource::SignalById(int id) const
{
    const auto it = m_idToIndex.find(id);
    if (it == m_idToIndex.end()) return nullptr;
    return &m_signals[static_cast<std::size_t>(it->second)];
}

std::uint64_t VcdLazyTraceSource::StartOffsetForTime(TimeValue t) const
{
    auto it = std::upper_bound(
        m_timeIndex.begin(), m_timeIndex.end(), t,
        [](TimeValue value, const TraceSidecarIndex::TimeSample& sample) {
            return value < sample.time;
        });
    if (it == m_timeIndex.begin()) return m_dataOffset;
    --it;
    return it->offset;
}

bool VcdLazyTraceSource::ParseValueChange(const std::string& line,
                                          const std::string& idCode,
                                          std::string& value) const
{
    if (line.empty()) return false;
    const char first = line[0];
    if (first == '0' || first == '1' || first == 'x' || first == 'z' ||
        first == 'X' || first == 'Z') {
        const std::string candidate = line.substr(1);
        if (candidate != idCode) return false;
        value.assign(1, static_cast<char>(
            std::tolower(static_cast<unsigned char>(first))));
        return true;
    }
    if (first == 'b' || first == 'r' || first == 's') {
        const std::size_t space = line.find(' ');
        if (space == std::string::npos) return false;
        const std::string candidate = line.substr(space + 1);
        if (candidate != idCode) return false;
        value = line.substr(1, space - 1);
        return true;
    }
    return false;
}

bool VcdLazyTraceSource::Query(const SignalInfo& signal, TimeValue t0, TimeValue t1,
                               std::vector<Transition>& out, std::string& error)
{
    out.clear();
    if (t1 < t0) return true;

    std::ifstream in(m_path, std::ios::binary);
    if (!in) {
        error = "unable to open " + m_path;
        return false;
    }
    in.seekg(static_cast<std::streamoff>(StartOffsetForTime(t0)));

    TimeValue currentTime = 0;
    std::string line;
    while (std::getline(in, line)) {
        TrimLine(line);
        if (line.empty()) continue;
        if (line[0] == '#') {
            currentTime = ParseTime(line.substr(1));
            if (currentTime > t1) break;
            continue;
        }
        if (line == "$dumpvars" || line == "$end" || line == "$comment") continue;
        std::string value;
        if (ParseValueChange(line, signal.idCode, value) && currentTime >= t0) {
            Transition transition{currentTime, std::move(value)};
            if (!out.empty() && out.back().time == currentTime) {
                out.back() = std::move(transition); // 同时间最后一次赋值生效
            } else {
                out.push_back(std::move(transition));
            }
        }
    }
    return true;
}

bool VcdLazyTraceSource::ValueAt(const SignalInfo& signal, TimeValue t,
                                 std::string& value, std::string& error)
{
    std::ifstream in(m_path, std::ios::binary);
    if (!in) {
        error = "unable to open " + m_path;
        return false;
    }
    in.seekg(static_cast<std::streamoff>(StartOffsetForTime(t)));

    TimeValue currentTime = 0;
    std::string last;
    std::string line;
    while (std::getline(in, line)) {
        TrimLine(line);
        if (line.empty()) continue;
        if (line[0] == '#') {
            currentTime = ParseTime(line.substr(1));
            if (currentTime > t) break;
            continue;
        }
        if (line == "$dumpvars" || line == "$end" || line == "$comment") continue;
        std::string parsed;
        if (ParseValueChange(line, signal.idCode, parsed)) last = std::move(parsed);
    }
    value = last.empty() ? "x" : last;
    return true;
}

} // namespace trace
} // namespace sigflow
