#pragma once
#include <eda/api/jobs.hpp>
#include <cstdint>
namespace eda::web {
// An execution request snapshot is not a snapshot of source-file contents.
struct ExecutionRequest {
    std::string jobId;
    JobRequest request;
    std::filesystem::path projectPath;
    std::filesystem::path workDir;
    std::filesystem::path toolRoot;
    std::filesystem::path dataDir;
    std::optional<std::string> inputSnapshotId;
    std::uint64_t maxOutputBytes = 0;
};
const char* StateName(JobState state);
bool IsTerminal(JobState state);
Json RecordToJson(const JobRecord& record);
Json ReportToJson(const JobReport& report);
Json RequestToJson(const ExecutionRequest& request);
Json ResultEvent(const JobReport& report);
// No real tool exit status is available; details are carried in diagnostics.
inline constexpr int kNoToolExitCode = -1;
}
