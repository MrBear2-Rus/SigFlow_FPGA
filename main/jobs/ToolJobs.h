#pragma once

#include "JobService.h"

#include <functional>
#include <vector>

struct JobCommand {
    wxString executable;
    std::vector<wxString> arguments;
    wxString workingDirectory;
    wxString logPath;
    int timeoutSeconds = 0;
    std::uint64_t maxOutputBytes = 0;
    std::uint64_t memoryLimitBytes = 0;
};

// 进程执行器：所有真实工具调用都经这里（内部走 PlatformProcess，不直接碰系统调用）。
class ToolJobExecutor {
public:
    // 状态机入口：确保 manifest 处于 Running，并执行确认门（FlashJob 用）。
    bool Begin(const ToolJob& job, const JobExecutionOptions& options, JobReport& report,
               wxString& errorMessage) const;

    // 终态收尾：推进 manifest 状态并落盘报告（成功/失败/超时都写）。
    void Finish(const ToolJob& job, JobReport& report) const;

    // 外部进程执行；combinedOutput 非空时回传合并后的 stdout+stderr（供日志解析）。
    bool Run(const ToolJob& job, const JobCommand& command, const JobExecutionOptions& options,
             JobReport& report, wxString& errorMessage,
             wxString* combinedOutput = nullptr) const;

    // 进程内运行体（如 SimulationEngine）：共用同一套状态机与报告落盘。
    bool RunInProcess(const ToolJob& job,
                      const std::function<bool(JobReport&, wxString&)>& runner,
                      const JobExecutionOptions& options, JobReport& report,
                      wxString& errorMessage) const;

private:
    static bool WriteReportLog(const wxString& path, const wxString& text);
};

struct SimulationJobRequest {
    wxString projectPath;
    wxString topModule;
    std::vector<wxString> sourceFiles;
    wxString executable;
    std::vector<wxString> arguments;
    wxString outputVcdPath;
    wxString workingDirectory;
    int timeoutSeconds = 0;
    // 编译型仿真（只出 DLL、不产 VCD）可关闭产物校验。
    bool requireVcd = true;
    // 进程内运行体（SimulationEngine 编译/运行）；设置后不再启动外部进程。
    std::function<bool(JobReport& report, wxString& errorMessage)> runner;
};

struct PackJobRequest {
    wxString projectPath;
    wxString pnrJsonPath;
    wxString bitstreamPath;
    wxString executable;
    wxString device;
    std::vector<wxString> arguments;
    wxString workingDirectory;
    int timeoutSeconds = 0;
};

struct FlashJobRequest {
    wxString projectPath;
    wxString bitstreamPath;
    wxString executable;
    std::vector<wxString> arguments;
    wxString workingDirectory;
    int timeoutSeconds = 0;
    bool requireConfirm = true;
};

struct SynthJobRequest {
    wxString projectPath;
    wxString topModule;
    std::vector<wxString> sourceFiles;
    wxString executable;
    std::vector<wxString> arguments;
    wxString scriptPath;
    wxString outputJsonPath;
    wxString workingDirectory;
    int timeoutSeconds = 0;
};

struct PnRJobRequest {
    wxString projectPath;
    wxString topModule;
    wxString executable;
    std::vector<wxString> arguments;
    wxString outputJsonPath;
    wxString workingDirectory;
    int timeoutSeconds = 0;
};

class SimulationJob {
public:
    bool Submit(const SimulationJobRequest& request, ToolJob& job,
                wxString& errorMessage) const;
    bool Execute(const SimulationJobRequest& request, const ToolJob& job,
                 const JobExecutionOptions& options, JobReport& report,
                 wxString& errorMessage) const;
};

class PackJob {
public:
    bool Submit(const PackJobRequest& request, ToolJob& job,
                wxString& errorMessage) const;
    bool Execute(const PackJobRequest& request, const ToolJob& job,
                 const JobExecutionOptions& options, JobReport& report,
                 wxString& errorMessage) const;
};

class FlashJob {
public:
    bool Submit(const FlashJobRequest& request, ToolJob& job,
                wxString& errorMessage) const;
    bool Execute(const FlashJobRequest& request, const ToolJob& job,
                 const JobExecutionOptions& options, JobReport& report,
                 wxString& errorMessage) const;
};

class SynthJob {
public:
    bool Submit(const SynthJobRequest& request, ToolJob& job,
                wxString& errorMessage) const;
    bool Execute(const SynthJobRequest& request, const ToolJob& job,
                 const JobExecutionOptions& options, JobReport& report,
                 wxString& errorMessage) const;
};

class PnRJob {
public:
    bool Submit(const PnRJobRequest& request, ToolJob& job,
                wxString& errorMessage) const;
    bool Execute(const PnRJobRequest& request, const ToolJob& job,
                 const JobExecutionOptions& options, JobReport& report,
                 wxString& errorMessage) const;
};
