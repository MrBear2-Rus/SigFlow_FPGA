#include "NextpnrLogModel.h"

#include <cctype>
#include <regex>
#include <sstream>
#include <string>

namespace eda {
namespace pnr {
namespace {

constexpr int kMaxLogLines = 50000;

std::string Trim(std::string value) {
    const std::size_t first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const std::size_t last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::string ToLower(std::string value) {
    for (char& c : value) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return value;
}

bool Contains(const std::string& haystack, const char* needle) {
    return haystack.find(needle) != std::string::npos;
}

bool StartsWith(const std::string& value, const char* prefix) {
    return value.rfind(prefix, 0) == 0;
}

struct ErrorEntry {
    const char* pattern;
    const char* desc;
    const char* suggestion;
};

// 与 main/fpga/NextpnrLogParser.cpp 的错误字典一致（顺序敏感）。
const ErrorEntry kErrorTable[] = {
    {"Failed to open CST file '(.+?)': no such file or directory", "CST constraint file missing",
     "Check cst= path in sigflow.project; generate CST under FPGA > Pin Constraints"},
    {"Failed to open '--json' file '(.+?)': no such file or directory", "JSON netlist missing",
     "Run FPGA > Synthesis first to produce <top>.json"},
    {"No package for partnumber (.+)", "Device not recognized by chip database",
     "Tang Nano 9K device is GW1NR-LV9QN88PC6/I5; check --device"},
    {"For the GW1N-9 series you need to specify --vopt family=", "Missing family option",
     "Add \"--vopt\", \"family=GW1N-9C\" to nextpnr_args"},
    {"Unconstrained IO:\\s*(.+)", "Top ports not bound to physical pins",
     "Add IO_LOC bindings for all top-level ports in CST"},
    {"Failed to parse JSON file '(.+?)'", "Corrupted JSON netlist",
     "Clean yosys/ and re-run Synthesis"},
    {"Unable to read chipdb (.+)", "Gowin chip database missing",
     "Ensure share/himbaechel/gowin/chipdb-GW1N-9C.bin exists"},
    {"unrecognized --vopt option", "Bad --vopt option name",
     "nextpnr-himbaechel only supports family and cst"},
    {"Invalid constraint: (.+)", "Invalid CST constraint syntax",
     "CST accepts only IO_LOC / IO_PORT; comments start with #"},
    {"Failed to route net '(.*?)'", "A net could not be routed",
     "Likely pin-induced congestion; adjust pin assignment"},
    {"Program finished normally", "nextpnr place and route completed",
     "Continue with Apicula packing to produce the .fs bitstream"},
};

constexpr int kErrorTableSize = sizeof(kErrorTable) / sizeof(kErrorTable[0]);

} // namespace

NextpnrLogParser::NextpnrLogParser() = default;

bool NextpnrLogParser::Parse(const std::string& stdoutRaw, const std::string& /*stderrRaw*/,
                             NextpnrRunRecord& record) {
    events_.clear();
    errors_.clear();
    record.parsed = false;
    record.packCompleted = false;
    record.placeCompleted = false;
    record.routeCompleted = false;
    record.resources.clear();

    if (stdoutRaw.empty()) {
        record.parsed = true;
        record.errorCount = 1;
        NextpnrLogEvent emptyError;
        emptyError.category = "error";
        emptyError.chineseDesc = "nextpnr produced no output";
        emptyError.suggestion = "Check the nextpnr executable and command line arguments";
        errors_.push_back(emptyError);
        record.classifiedErrors = errors_;
        return false;
    }

    std::string normalized = stdoutRaw;
    for (std::size_t pos = normalized.find("\r\n"); pos != std::string::npos;
         pos = normalized.find("\r\n", pos)) {
        normalized.replace(pos, 2, "\n");
    }
    for (std::size_t pos = normalized.find('\r'); pos != std::string::npos;
         pos = normalized.find('\r', pos)) {
        normalized.replace(pos, 1, "\n");
    }

    std::istringstream stream(normalized);
    std::string rawLine;
    int lineNumber = 0;
    bool truncated = false;

    while (std::getline(stream, rawLine)) {
        if (lineNumber >= kMaxLogLines) {
            truncated = true;
            break;
        }
        const std::string line = Trim(rawLine);
        ++lineNumber;
        if (line.empty()) continue;

        NextpnrLogEvent event;
        event.rawLine = line;
        event.lineNumber = lineNumber;
        event.stage = DetectStage(line);

        if (StartsWith(line, "ERROR:") || StartsWith(line, "Error:")) {
            event.category = "error";
            ++record.errorCount;
            ClassifyError(line, lineNumber);
        } else if (StartsWith(line, "Warning:") || StartsWith(line, "WARNING:")) {
            event.category = "warning";
            ++record.warningCount;
        } else if (event.stage != "unknown") {
            event.category = "stage";
        } else {
            event.category = "info";
        }

        if (record.toolVersion.empty() || record.toolVersion == "unknown") {
            const std::string version = ExtractVersion(line);
            if (version != "unknown") record.toolVersion = version;
        }

        ExtractDeviceInfo(line, record);
        ExtractTimingInfo(line, record);
        ParseResourceLine(line, record);

        events_.push_back(event);
    }

    NextpnrLogParser::ExtractFinalCounts(normalized, record);

    if (truncated) {
        ++record.warningCount;
        NextpnrLogEvent truncation;
        truncation.category = "warning";
        truncation.chineseDesc = "log truncated";
        truncation.suggestion = "over " + std::to_string(kMaxLogLines) +
                                " lines; only the first " + std::to_string(kMaxLogLines) +
                                " were parsed";
        errors_.push_back(truncation);
    }

    record.parsed = true;
    record.classifiedErrors = errors_;
    return true;
}

std::string NextpnrLogParser::DetectStage(const std::string& line) {
    const std::string lower = ToLower(line);
    if (Contains(lower, "packing") || Contains(lower, "pack iob") || Contains(lower, "pack io") ||
        Contains(lower, "pack gsr") || Contains(lower, "pack wide") ||
        Contains(lower, "pack alu") || Contains(lower, "pack pll") ||
        Contains(lower, "pack lut") || Contains(lower, "pack constant") ||
        Contains(lower, "pack cell")) {
        return "pack";
    }
    if (Contains(lower, "placing") || Contains(lower, "placer") ||
        Contains(lower, "placement") || Contains(lower, "simulated annealing")) {
        return "place";
    }
    if (Contains(lower, "routing") || Contains(lower, "router1") ||
        Contains(lower, "router2") || Contains(lower, "route complete")) {
        return "route";
    }
    if (Contains(lower, "device utilisation") || Contains(lower, "device utilization")) {
        return "resource";
    }
    if (Contains(lower, "max frequency") || Contains(lower, "critical path") ||
        Contains(lower, "slack histogram")) {
        return "timing";
    }
    return "unknown";
}

bool NextpnrLogParser::ParseResourceLine(const std::string& line, NextpnrRunRecord& record) {
    static const std::regex re("^\\s*(\\w+)\\s*:\\s*(\\d+)\\s*/\\s*(\\d+)\\s+(\\d+)%");
    std::smatch match;
    if (!std::regex_search(line, match, re)) return false;

    NextpnrRunRecord::ResourceUsage usage;
    usage.used = std::stoi(match[2].str());
    usage.total = std::stoi(match[3].str());
    usage.percent = static_cast<double>(std::stoi(match[4].str()));
    record.resources[match[1].str()] = usage;
    return true;
}

void NextpnrLogParser::ClassifyError(const std::string& line, int lineNumber) {
    for (int i = 0; i < kErrorTableSize; ++i) {
        const std::regex re(kErrorTable[i].pattern, std::regex::ECMAScript | std::regex::icase);
        std::smatch match;
        if (std::regex_search(line, match, re)) {
            NextpnrLogEvent error;
            error.rawLine = line;
            error.lineNumber = lineNumber;
            error.category = "error";
            error.chineseDesc = kErrorTable[i].desc;
            error.suggestion = kErrorTable[i].suggestion;
            if (match.size() >= 2) {
                const std::string detail = Trim(match[1].str());
                if (!detail.empty() && detail.size() < 200) error.extractedDetail = detail;
            }
            errors_.push_back(error);
            return;
        }
    }

    NextpnrLogEvent unknown;
    unknown.rawLine = line;
    unknown.lineNumber = lineNumber;
    unknown.category = "error";
    unknown.chineseDesc = "unrecognized nextpnr error; inspect the raw log";
    unknown.suggestion = "Consult nextpnr documentation or report the issue";
    errors_.push_back(unknown);
}

std::string NextpnrLogParser::ExtractVersion(const std::string& line) {
    static const std::regex re("Version\\s+([0-9][0-9A-Za-z._+-]*)");
    std::smatch match;
    if (std::regex_search(line, match, re)) return match[1].str();
    return "unknown";
}

void NextpnrLogParser::ExtractDeviceInfo(const std::string& line, NextpnrRunRecord& record) {
    static const std::regex re("Using uarch '([^']+)' for device '([^']+)'");
    std::smatch match;
    if (std::regex_search(line, match, re)) record.deviceName = match[2].str();
}

void NextpnrLogParser::ExtractTimingInfo(const std::string& line, NextpnrRunRecord& record) {
    static const std::regex re(
        "Max frequency for clock '([^']+)':\\s*([\\d.]+)\\s*MHz\\s*\\((PASS|FAIL)");
    std::smatch match;
    if (std::regex_search(line, match, re)) {
        record.clockName = match[1].str();
        record.maxFrequencyMHz = std::stod(match[2].str());
        record.timingPassed = (match[3].str() == "PASS");
    }

    const std::string lower = ToLower(line);
    if (Contains(lower, "packing") || Contains(lower, "pack iob") || Contains(lower, "pack io") ||
        Contains(lower, "pack gsr")) {
        record.packCompleted = true;
    }
    if (Contains(lower, "placing") || Contains(lower, "placer") ||
        Contains(lower, "analytic placement") || Contains(lower, "simulated annealing")) {
        record.placeCompleted = true;
    }
    if (Contains(lower, "routing") || Contains(lower, "router1")) {
        record.routeCompleted = true;
    }
}

void NextpnrLogParser::ExtractFinalCounts(const std::string& fullText, NextpnrRunRecord& record) {
    static const std::regex re("(\\d+)\\s+warnings?,\\s*(\\d+)\\s+errors?",
                               std::regex::ECMAScript | std::regex::icase);
    std::smatch match;
    if (std::regex_search(fullText, match, re)) {
        record.warningCount = std::stoi(match[1].str());
        record.errorCount = std::stoi(match[2].str());
    }
}

} // namespace pnr
} // namespace eda
