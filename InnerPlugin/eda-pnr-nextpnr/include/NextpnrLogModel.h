#pragma once

#include <map>
#include <string>
#include <vector>

namespace eda {
namespace pnr {

struct NextpnrLogEvent {
    std::string rawLine;
    std::string category;
    std::string stage;
    std::string chineseDesc;
    std::string suggestion;
    std::string extractedDetail;
    int lineNumber = 0;
};

struct NextpnrRunRecord {
    std::string toolName;
    std::string toolVersion;
    std::string deviceName;
    std::string familyName;
    std::string executablePath;
    std::string workingDir;
    std::string arguments;

    std::string startTime;   // ISO-8601（由执行器填充，解析器不改）
    std::string endTime;     // ISO-8601
    int exitCode = -1;
    long processId = 0;

    std::string stdoutRaw;
    std::string stderrRaw;

    bool parsed = false;
    bool packCompleted = false;
    bool placeCompleted = false;
    bool routeCompleted = false;

    double maxFrequencyMHz = 0.0;
    std::string clockName;
    bool timingPassed = false;

    struct ResourceUsage {
        int used = 0;
        int total = 0;
        double percent = 0.0;
    };
    std::map<std::string, ResourceUsage> resources;

    std::vector<NextpnrLogEvent> classifiedErrors;

    int warningCount = 0;
    int errorCount = 0;
};

// 自 `main/fpga/NextpnrLogParser` 移入并去 wx 化（std::regex / std::string）。
class NextpnrLogParser {
public:
    NextpnrLogParser();

    bool Parse(const std::string& stdoutRaw, const std::string& stderrRaw,
               NextpnrRunRecord& record);

    const std::vector<NextpnrLogEvent>& GetEvents() const { return events_; }
    const std::vector<NextpnrLogEvent>& GetErrors() const { return errors_; }

    static void ExtractFinalCounts(const std::string& fullText, NextpnrRunRecord& record);

private:
    std::string DetectStage(const std::string& line);
    bool ParseResourceLine(const std::string& line, NextpnrRunRecord& record);
    void ClassifyError(const std::string& line, int lineNumber);
    std::string ExtractVersion(const std::string& firstLine);
    void ExtractDeviceInfo(const std::string& line, NextpnrRunRecord& record);
    void ExtractTimingInfo(const std::string& line, NextpnrRunRecord& record);

    std::vector<NextpnrLogEvent> events_;
    std::vector<NextpnrLogEvent> errors_;
};

} // namespace pnr
} // namespace eda
