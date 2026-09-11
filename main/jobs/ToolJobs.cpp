#include "ToolJobs.h"

#include "PlatformProcess.h"

#include "../fpga/ArtifactValidator.h"
#include "../fpga/FpgaPackService.h"
#include "../fpga/FpgaYosysLogParser.h"
#include "../fpga/NextpnrLogParser.h"

#include <wx/datetime.h>
#include <wx/file.h>
#include <wx/dir.h>
#include <wx/filename.h>

#include <chrono>
#include <memory>
#include <mutex>

namespace {

wxString NowUtc()
{
    return wxDateTime::Now().ToUTC().FormatISOCombined('T') + "Z";
}

bool Exists(const wxString& path)
{
    return !path.IsEmpty() && wxFileExists(path);
}

Json::Value StringArray(const std::vector<wxString>& values)
{
    Json::Value array(Json::arrayValue);
    for (const wxString& value : values) array.append(value.ToStdString());
    return array;
}

Json::Value ToJson(const SimulationJobRequest& request)
{
    Json::Value parameters(Json::objectValue);
    parameters["top_module"] = request.topModule.ToStdString();
    parameters["executable"] = request.executable.ToStdString();
    parameters["output_vcd"] = request.outputVcdPath.ToStdString();
    parameters["working_directory"] = request.workingDirectory.ToStdString();
    parameters["timeout_seconds"] = request.timeoutSeconds;
    parameters["source_files"] = StringArray(request.sourceFiles);
    parameters["arguments"] = StringArray(request.arguments);
    return parameters;
}

Json::Value ToJson(const PackJobRequest& request)
{
    Json::Value parameters(Json::objectValue);
    parameters["pnr_json"] = request.pnrJsonPath.ToStdString();
    parameters["bitstream"] = request.bitstreamPath.ToStdString();
    parameters["executable"] = request.executable.ToStdString();
    parameters["device"] = request.device.ToStdString();
    parameters["working_directory"] = request.workingDirectory.ToStdString();
    parameters["timeout_seconds"] = request.timeoutSeconds;
    parameters["arguments"] = StringArray(request.arguments);
    return parameters;
}

Json::Value ToJson(const FlashJobRequest& request)
{
    Json::Value parameters(Json::objectValue);
    parameters["bitstream"] = request.bitstreamPath.ToStdString();
    parameters["executable"] = request.executable.ToStdString();
    parameters["working_directory"] = request.workingDirectory.ToStdString();
    parameters["timeout_seconds"] = request.timeoutSeconds;
    parameters["require_confirm"] = request.requireConfirm;
    parameters["arguments"] = StringArray(request.arguments);
    return parameters;
}

Json::Value ToJson(const SynthJobRequest& request)
{
    Json::Value parameters(Json::objectValue);
    parameters["top_module"] = request.topModule.ToStdString();
    parameters["executable"] = request.executable.ToStdString();
    parameters["script_path"] = request.scriptPath.ToStdString();
    parameters["output_json"] = request.outputJsonPath.ToStdString();
    parameters["working_directory"] = request.workingDirectory.ToStdString();
    parameters["timeout_seconds"] = request.timeoutSeconds;
    parameters["source_files"] = StringArray(request.sourceFiles);
    parameters["arguments"] = StringArray(request.arguments);
    return parameters;
}

Json::Value ToJson(const PnRJobRequest& request)
{
    Json::Value parameters(Json::objectValue);
    parameters["top_module"] = request.topModule.ToStdString();
    parameters["executable"] = request.executable.ToStdString();
    parameters["output_json"] = request.outputJsonPath.ToStdString();
    parameters["working_directory"] = request.workingDirectory.ToStdString();
    parameters["timeout_seconds"] = request.timeoutSeconds;
    parameters["arguments"] = StringArray(request.arguments);
    return parameters;
}

ToolJobRequest BaseRequest(ToolJobType type, const wxString& projectPath,
                           const Json::Value& parameters)
{
    ToolJobRequest request;
    request.type = type;
    request.projectPath = projectPath;
    request.parameters = parameters;
    return request;
}

// 状态机由执行器驱动：把 manifest 推进到终态（非法/已终态时忽略，报告仍是权威记录）。
void FinishJob(const ToolJob& job, ToolJobState state, int exitCode, const wxString& reason)
{
    ToolJob current;
    wxString ignored;
    if (!JobService().Load(job.request.projectPath, job.id, current, ignored)) return;
    if (IsTerminalToolJobState(current.state)) return;
    JobService().Transition(job.request.projectPath, job.id, state, reason, exitCode, ignored);
}

wxString YosysCoordinate(const YosysLogEvent& event)
{
    if (event.sourceFile.IsEmpty()) return wxString();
    return event.sourceFile + wxString::Format(":%d", event.sourceLine);
}

void AppendYosysErrors(const wxString& combinedLog, JobReport& report)
{
    if (combinedLog.IsEmpty()) return;
    const YosysLogRecord record = FpgaYosysLogParser().Parse(combinedLog);
    for (const YosysLogEvent& event : record.events) {
        if (event.severity == YosysLogSeverity::Info) continue;
        JobError error;
        error.code = event.ruleId.IsEmpty() ? wxString("YOSYS_DIAGNOSTIC") : event.ruleId;
        error.severity = event.severity == YosysLogSeverity::Error ? "error" : "warning";
        error.stage = event.stage.IsEmpty() ? wxString("synthesis") : event.stage;
        error.irCoordinate = YosysCoordinate(event);
        error.summary = event.evidence.IsEmpty() ? event.rawLine : event.evidence;
        error.logLine = event.lineNumber;
        report.errors.push_back(error);
    }
}

void AppendNextpnrErrors(const wxString& combinedLog, JobReport& report, double& maxFrequencyMHz)
{
    if (combinedLog.IsEmpty()) return;
    NextpnrLogParser parser;
    NextpnrRunRecord record;
    parser.Parse(combinedLog, wxString(), record);
    maxFrequencyMHz = record.maxFrequencyMHz;
    for (const NextpnrLogEvent& event : parser.GetErrors()) {
        JobError error;
        error.code = event.category.IsEmpty() ? wxString("NEXTPNR_DIAGNOSTIC") : event.category;
        error.severity = event.category == "error" ? "error" : "warning";
        error.stage = event.stage.IsEmpty() ? wxString("pnr") : event.stage;
        error.summary = event.chineseDesc.IsEmpty() ? event.rawLine : event.chineseDesc;
        error.logLine = event.lineNumber;
        report.errors.push_back(error);
    }
}

} // namespace

bool ToolJobExecutor::WriteReportLog(const wxString& path, const wxString& text)
{
    if (path.IsEmpty()) return true;
    wxFileName fileName(path);
    if (!fileName.GetPath().IsEmpty() &&
        !wxDirExists(fileName.GetPath()) &&
        !wxFileName::Mkdir(fileName.GetPath(), wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL)) return false;
    wxFile file(path, wxFile::write);
    if (!file.IsOpened()) return false;
    const wxScopedCharBuffer utf8 = text.ToUTF8();
    return utf8.data() && file.Write(utf8.data(), utf8.length()) ==
        static_cast<wxFileOffset>(utf8.length());
}

bool ToolJobExecutor::Begin(const ToolJob& job, const JobExecutionOptions& options,
                            JobReport& report, wxString& errorMessage) const
{
    report = JobReport();
    report.jobType = job.request.type;
    report.jobId = job.id;
    report.state = ToolJobState::Running;
    report.startedAt = NowUtc();

    // 进入执行前确保 manifest 处于 Running（顺带执行并发上限校验）。
    ToolJob current;
    wxString stateError;
    if (JobService().Load(job.request.projectPath, job.id, current, stateError) &&
        current.state != ToolJobState::Running &&
        !JobService().Start(job.request.projectPath, job.id, stateError)) {
        errorMessage = stateError;
        report.state = ToolJobState::Failed;
        report.errors.push_back({ "JOB_NOT_STARTABLE", "error", "Validating", "",
                                  errorMessage, 0 });
        Finish(job, report);
        return false;
    }
    if (options.requireConfirm && (!options.confirm || !options.confirm())) {
        errorMessage = "Job execution was not confirmed by the user.";
        report.state = ToolJobState::Cancelled;
        report.errors.push_back({ "USER_CONFIRMATION_REQUIRED", "warning", "Validating", "",
                                  errorMessage, 0 });
        Finish(job, report);
        return false;
    }
    return true;
}

void ToolJobExecutor::Finish(const ToolJob& job, JobReport& report) const
{
    ToolJob current;
    wxString stateError;
    if (JobService().Load(job.request.projectPath, job.id, current, stateError) &&
        IsTerminalToolJobState(current.state) && current.state != report.state) {
        report.state = current.state;
        if (report.exitCode == 0 && current.exitCode != 0) report.exitCode = current.exitCode;
    }
    if (report.completedAt.IsEmpty()) report.completedAt = NowUtc();
    if (report.summary.IsEmpty()) report.summary = ToString(report.state);
    FinishJob(job, report.state, report.exitCode, report.summary);
    wxString ignored;
    JobService().WriteReport(job.request.projectPath, job.id, report, ignored);
    if (IsTerminalToolJobState(report.state)) {
        JobService::ClearCancelRequested(job.request.projectPath, job.id);
    }
}

bool ToolJobExecutor::Run(const ToolJob& job, const JobCommand& command,
                          const JobExecutionOptions& options, JobReport& report,
                          wxString& errorMessage, wxString* combinedOutput) const
{
    if (!Begin(job, options, report, errorMessage)) return false;

    if (!Exists(command.executable)) {
        errorMessage = "Job executable does not exist: " + command.executable;
        report.state = ToolJobState::Failed;
        report.summary = errorMessage;
        report.errors.push_back({ "EXECUTABLE_NOT_FOUND", "error", "Validating", "",
                                  errorMessage, 0 });
        Finish(job, report);
        return false;
    }

    PlatformProcessRequest process;
    process.executable = command.executable;
    process.arguments = command.arguments;
    process.workingDirectory = command.workingDirectory;
    process.timeoutSeconds = command.timeoutSeconds > 0
        ? command.timeoutSeconds : DefaultJobTimeoutSeconds(job.request.type);
    process.maxOutputBytes = options.maxOutputBytes > 0
        ? options.maxOutputBytes : command.maxOutputBytes;
    process.memoryLimitBytes = options.memoryLimitBytes > 0
        ? options.memoryLimitBytes : command.memoryLimitBytes;
    process.onStarted = [&job](void* handle) {
        JobService::RegisterProcess(job.request.projectPath, job.id, handle);
    };

    wxString output;
    std::mutex outputMutex;
    const auto onOutput = [&](const wxString& chunk, bool isError) {
        {
            std::lock_guard<std::mutex> lock(outputMutex);
            output += chunk;
        }
        if (options.output) options.output(chunk, isError);
    };

    const auto start = std::chrono::steady_clock::now();
    const PlatformProcessResult result = PlatformProcess::Run(process, onOutput);
    JobService::UnregisterProcess(job.request.projectPath, job.id);
    const bool cancelRequested = JobService::IsCancelRequested(
        job.request.projectPath, job.id);
    const auto end = std::chrono::steady_clock::now();
    report.durationMs = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    report.completedAt = NowUtc();
    report.exitCode = result.exitCode;

    if (cancelRequested) {
        errorMessage = "Job cancelled by the user.";
        report.state = ToolJobState::Cancelled;
        report.errors.push_back({ "JOB_CANCELLED", "warning", "Running", "",
                                  errorMessage, 0 });
    } else if (!result.started) {
        errorMessage = result.errorMessage;
        report.state = ToolJobState::Failed;
        report.errors.push_back({ "PROCESS_START_FAILED", "error", "Running", "",
                                  errorMessage, 0 });
    } else if (result.timedOut) {
        errorMessage = "Job timed out.";
        report.state = ToolJobState::TimedOut;
        report.errors.push_back({ "JOB_TIMEOUT", "error", "Running", "", errorMessage, 0 });
    } else if (result.exitCode != 0) {
        errorMessage = wxString::Format("Job process exited with code %d.", result.exitCode);
        report.state = ToolJobState::Failed;
        report.errors.push_back({ "PROCESS_EXIT_NONZERO", "error", "Running", "",
                                  errorMessage, 0 });
    } else {
        report.state = ToolJobState::ValidatingArtifact;
        report.summary = "Job process completed.";
    }
    if (result.outputTruncated) {
        report.errors.push_back({ "LOG_OUTPUT_TRUNCATED", "warning", "Reporting", "",
                                  "The process log exceeded the configured output limit.", 0 });
    }
    if (!command.logPath.IsEmpty() && !WriteReportLog(command.logPath, output)) {
        report.errors.push_back({ "LOG_WRITE_FAILED", "warning", "Reporting", "",
                                  "Unable to write job execution log.", 0 });
    }
    if (combinedOutput != nullptr) *combinedOutput = output;
    if (report.summary.IsEmpty()) report.summary = errorMessage;
    Finish(job, report);
    return report.state == ToolJobState::ValidatingArtifact;
}

bool ToolJobExecutor::RunInProcess(const ToolJob& job,
                                   const std::function<bool(JobReport&, wxString&)>& runner,
                                   const JobExecutionOptions& options, JobReport& report,
                                   wxString& errorMessage) const
{
    if (!Begin(job, options, report, errorMessage)) return false;
    const auto start = std::chrono::steady_clock::now();
    const bool ok = runner && runner(report, errorMessage);
    const auto end = std::chrono::steady_clock::now();
    report.durationMs = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    report.completedAt = NowUtc();
    if (report.state == ToolJobState::Running) {
        report.state = ok ? ToolJobState::ValidatingArtifact : ToolJobState::Failed;
    }
    if (!ok && report.errors.empty()) {
        report.errors.push_back({ "IN_PROCESS_FAILED", "error", "Running", "",
                                  errorMessage, 0 });
    }
    if (JobService::IsCancelRequested(job.request.projectPath, job.id)) {
        report.state = ToolJobState::Cancelled;
        errorMessage = "Job cancelled by the user.";
        report.errors.push_back({ "JOB_CANCELLED", "warning", "Running", "",
                                  errorMessage, 0 });
    }
    report.summary = errorMessage.IsEmpty() ? wxString("Job completed.") : errorMessage;
    Finish(job, report);
    return ok && report.state == ToolJobState::ValidatingArtifact;
}

bool SimulationJob::Submit(const SimulationJobRequest& request, ToolJob& job,
                           wxString& errorMessage) const
{
    const bool hasRunner = static_cast<bool>(request.runner);
    if (request.projectPath.IsEmpty() || request.topModule.IsEmpty() ||
        request.sourceFiles.empty() ||
        (request.requireVcd && request.outputVcdPath.IsEmpty()) ||
        (!hasRunner && request.executable.IsEmpty())) {
        errorMessage = "Simulation Job requires project, top module, sources, "
                       "and an executable or an in-process runner.";
        return false;
    }
    return JobService().Create(BaseRequest(ToolJobType::Simulation, request.projectPath,
                                           ToJson(request)), job, errorMessage);
}

bool SimulationJob::Execute(const SimulationJobRequest& request, const ToolJob& job,
                            const JobExecutionOptions& options, JobReport& report,
                            wxString& errorMessage) const
{
    if (request.runner) {
        if (!ToolJobExecutor().RunInProcess(job, request.runner, options, report, errorMessage)) {
            return false;
        }
    } else {
        JobCommand command{ request.executable, request.arguments, request.workingDirectory,
                            JobService::GetPaths(request.projectPath, ToolJobType::Simulation,
                                                 job.id).logs + wxFileName::GetPathSeparator() +
                                "process.log",
                            request.timeoutSeconds };
        if (!ToolJobExecutor().Run(job, command, options, report, errorMessage)) return false;
    }
    if (!request.requireVcd) {
        report.state = ToolJobState::Succeeded;
        report.summary = "Simulation model compiled.";
        FinishJob(job, ToolJobState::Succeeded, report.exitCode, report.summary);
        return JobService().WriteReport(request.projectPath, job.id, report, errorMessage);
    }
    if (!Exists(request.outputVcdPath)) {
        errorMessage = "Simulation completed without producing a VCD artifact.";
        report.state = ToolJobState::Failed;
        report.summary = errorMessage;
        report.errors.push_back({ "VCD_NOT_FOUND", "error", "ValidatingArtifact", "",
                                  errorMessage, 0 });
        ToolJobExecutor().Finish(job, report);
        return false;
    }
    if (!JobService().RecordArtifact(request.projectPath, job.id, request.outputVcdPath,
                                     "vcd", errorMessage)) return false;
    // RecordArtifact 已把产物写进报告文件，必须回读合并后再定稿，否则产物登记会被覆盖。
    JobReport persisted;
    if (!JobService().LoadReport(request.projectPath, job.id, persisted, errorMessage)) return false;
    persisted.state = ToolJobState::Succeeded;
    persisted.summary = "Simulation completed and VCD artifact recorded.";
    report = persisted;
    FinishJob(job, ToolJobState::Succeeded, report.exitCode, report.summary);
    return JobService().WriteReport(request.projectPath, job.id, report, errorMessage);
}

bool PackJob::Submit(const PackJobRequest& request, ToolJob& job,
                     wxString& errorMessage) const
{
    if (request.projectPath.IsEmpty() || request.pnrJsonPath.IsEmpty() ||
        request.bitstreamPath.IsEmpty() || request.executable.IsEmpty()) {
        errorMessage = "Pack Job requires project, PnR JSON, bitstream, and executable.";
        return false;
    }
    if (!FpgaPackService().ValidateInput(request.pnrJsonPath, errorMessage)) return false;
    return JobService().Create(BaseRequest(ToolJobType::Pack, request.projectPath,
                                           ToJson(request)), job, errorMessage);
}

bool PackJob::Execute(const PackJobRequest& request, const ToolJob& job,
                      const JobExecutionOptions& options, JobReport& report,
                      wxString& errorMessage) const
{
    JobCommand command{ request.executable, request.arguments, request.workingDirectory,
                        JobService().GetPaths(request.projectPath, ToolJobType::Pack,
                                             job.id).logs + wxFileName::GetPathSeparator() +
                            "process.log",
                        request.timeoutSeconds };
    if (!ToolJobExecutor().Run(job, command, options, report, errorMessage)) return false;
    FpgaPackReport packReport;
    FpgaPackRequest packRequest{ request.pnrJsonPath, request.bitstreamPath,
                                 request.executable, request.device };
    if (!FpgaPackService().Finalize(packRequest, 0, packReport) || !packReport.success) {
        errorMessage = packReport.message;
        report.state = ToolJobState::Failed;
        report.summary = errorMessage;
        report.errors.push_back({ "PACK_ARTIFACT_INVALID", "error", "ValidatingArtifact", "",
                                  errorMessage, 0 });
        ToolJobExecutor().Finish(job, report);
        return false;
    }
    if (!JobService().RecordArtifact(request.projectPath, job.id, packReport.bitstreamPath,
                                     "bitstream", errorMessage)) {
        report.state = ToolJobState::Failed;
        report.summary = errorMessage;
        report.errors.push_back({ "PACK_ARTIFACT_UNSAFE", "error", "ValidatingArtifact", "",
                                  errorMessage, 0 });
        ToolJobExecutor().Finish(job, report);
        return false;
    }
    JobReport persisted;
    if (!JobService().LoadReport(request.projectPath, job.id, persisted, errorMessage)) {
        report.state = ToolJobState::Failed;
        report.summary = errorMessage;
        ToolJobExecutor().Finish(job, report);
        return false;
    }
    report = persisted;
    report.state = ToolJobState::Succeeded;
    report.summary = "FPGA bitstream packed and validated.";
    FinishJob(job, ToolJobState::Succeeded, report.exitCode, report.summary);
    return JobService().WriteReport(request.projectPath, job.id, report, errorMessage);
}

bool FlashJob::Submit(const FlashJobRequest& request, ToolJob& job,
                      wxString& errorMessage) const
{
    if (request.projectPath.IsEmpty() || request.bitstreamPath.IsEmpty() ||
        request.executable.IsEmpty() || !Exists(request.bitstreamPath)) {
        errorMessage = "Flash Job requires project, existing bitstream, and executable.";
        return false;
    }
    return JobService().Create(BaseRequest(ToolJobType::Flash, request.projectPath,
                                           ToJson(request)), job, errorMessage);
}

bool FlashJob::Execute(const FlashJobRequest& request, const ToolJob& job,
                       const JobExecutionOptions& options, JobReport& report,
                       wxString& errorMessage) const
{
    JobExecutionOptions guarded = options;
    guarded.requireConfirm = request.requireConfirm || options.requireConfirm;
    JobCommand command{ request.executable, request.arguments, request.workingDirectory,
                        JobService().GetPaths(request.projectPath, ToolJobType::Flash,
                                             job.id).logs + wxFileName::GetPathSeparator() +
                            "process.log",
                        request.timeoutSeconds };
    if (!ToolJobExecutor().Run(job, command, guarded, report, errorMessage)) return false;
    if (!Exists(request.bitstreamPath)) {
        errorMessage = "The FPGA bitstream disappeared before the flash completed.";
        report.state = ToolJobState::Failed;
        report.summary = errorMessage;
        report.errors.push_back({ "FLASH_ARTIFACT_MISSING", "error", "ValidatingArtifact", "",
                                  errorMessage, 0 });
        ToolJobExecutor().Finish(job, report);
        return false;
    }
    if (!JobService().RecordArtifact(request.projectPath, job.id, request.bitstreamPath,
                                     "bitstream", errorMessage)) {
        report.state = ToolJobState::Failed;
        report.summary = errorMessage;
        report.errors.push_back({ "FLASH_ARTIFACT_UNSAFE", "error", "ValidatingArtifact", "",
                                  errorMessage, 0 });
        ToolJobExecutor().Finish(job, report);
        return false;
    }
    JobReport persisted;
    if (!JobService().LoadReport(request.projectPath, job.id, persisted, errorMessage)) {
        report.state = ToolJobState::Failed;
        report.summary = errorMessage;
        ToolJobExecutor().Finish(job, report);
        return false;
    }
    report = persisted;
    report.state = ToolJobState::Succeeded;
    report.summary = "FPGA flash completed after confirmation.";
    FinishJob(job, ToolJobState::Succeeded, report.exitCode, report.summary);
    return JobService().WriteReport(request.projectPath, job.id, report, errorMessage);
}

bool SynthJob::Submit(const SynthJobRequest& request, ToolJob& job,
                          wxString& errorMessage) const
{
    if (request.projectPath.IsEmpty() || request.topModule.IsEmpty() ||
        request.executable.IsEmpty() || request.outputJsonPath.IsEmpty()) {
        errorMessage = "Synthesis Job requires project, top module, executable, and output JSON.";
        return false;
    }
    for (const wxString& source : request.sourceFiles) {
        if (!Exists(source)) {
            errorMessage = "RTL source does not exist: " + source;
            return false;
        }
    }
    if (!request.scriptPath.IsEmpty() && !Exists(request.scriptPath)) {
        errorMessage = "Yosys script does not exist: " + request.scriptPath;
        return false;
    }
    return JobService().Create(BaseRequest(ToolJobType::Synthesis, request.projectPath,
                                           ToJson(request)), job, errorMessage);
}

bool SynthJob::Execute(const SynthJobRequest& request, const ToolJob& job,
                           const JobExecutionOptions& options, JobReport& report,
                           wxString& errorMessage) const
{
    JobCommand command{ request.executable, request.arguments, request.workingDirectory,
                        JobService().GetPaths(request.projectPath, ToolJobType::Synthesis,
                                             job.id).logs + wxFileName::GetPathSeparator() +
                            "process.log",
                        request.timeoutSeconds };
    wxString combinedOutput;
    if (!ToolJobExecutor().Run(job, command, options, report, errorMessage, &combinedOutput)) {
        return false;
    }
    AppendYosysErrors(combinedOutput, report);
    ArtifactValidator validator;
    NetlistArtifactReport artifactReport;
    if (!validator.ValidateYosysJson(request.outputJsonPath, request.topModule, artifactReport) ||
        artifactReport.status != ArtifactValidationStatus::Valid) {
        errorMessage = artifactReport.message.IsEmpty()
            ? wxString("Synthesis did not produce a valid netlist artifact.")
            : artifactReport.message;
        report.state = ToolJobState::Failed;
        report.summary = errorMessage;
        report.errors.push_back({ "SYN_ARTIFACT_INVALID", "error", "ValidatingArtifact", "",
                                  errorMessage, 0 });
        ToolJobExecutor().Finish(job, report);
        return false;
    }
    if (!JobService().RecordArtifact(request.projectPath, job.id, artifactReport.path,
                                     "netlist", errorMessage)) {
        report.state = ToolJobState::Failed;
        report.summary = errorMessage;
        report.errors.push_back({ "SYN_ARTIFACT_UNSAFE", "error", "ValidatingArtifact", "",
                                  errorMessage, 0 });
        ToolJobExecutor().Finish(job, report);
        return false;
    }
    JobReport persisted;
    if (!JobService().LoadReport(request.projectPath, job.id, persisted, errorMessage)) {
        report.state = ToolJobState::Failed;
        report.summary = errorMessage;
        ToolJobExecutor().Finish(job, report);
        return false;
    }
    report = persisted;
    report.state = ToolJobState::Succeeded;
    report.summary = wxString::Format("Synthesis completed: %llu cells, %llu ports.",
                                      static_cast<unsigned long long>(artifactReport.cellCount),
                                      static_cast<unsigned long long>(artifactReport.portCount));
    FinishJob(job, ToolJobState::Succeeded, report.exitCode, report.summary);
    return JobService().WriteReport(request.projectPath, job.id, report, errorMessage);
}

bool PnRJob::Submit(const PnRJobRequest& request, ToolJob& job, wxString& errorMessage) const
{
    if (request.projectPath.IsEmpty() || request.topModule.IsEmpty() ||
        request.executable.IsEmpty() || request.outputJsonPath.IsEmpty()) {
        errorMessage = "PnR Job requires project, top module, executable, and output JSON.";
        return false;
    }
    return JobService().Create(BaseRequest(ToolJobType::PnR, request.projectPath,
                                           ToJson(request)), job, errorMessage);
}

bool PnRJob::Execute(const PnRJobRequest& request, const ToolJob& job,
                     const JobExecutionOptions& options, JobReport& report,
                     wxString& errorMessage) const
{
    JobCommand command{ request.executable, request.arguments, request.workingDirectory,
                        JobService().GetPaths(request.projectPath, ToolJobType::PnR,
                                             job.id).logs + wxFileName::GetPathSeparator() +
                            "process.log",
                        request.timeoutSeconds };
    wxString combinedOutput;
    if (!ToolJobExecutor().Run(job, command, options, report, errorMessage, &combinedOutput)) {
        return false;
    }
    double maxFrequencyMHz = 0.0;
    AppendNextpnrErrors(combinedOutput, report, maxFrequencyMHz);
    const wxULongLong size = wxFileName(request.outputJsonPath).GetSize();
    if (!Exists(request.outputJsonPath) || size == wxInvalidSize || size.GetValue() == 0) {
        errorMessage = "PnR did not produce a placed netlist artifact: " + request.outputJsonPath;
        report.state = ToolJobState::Failed;
        report.summary = errorMessage;
        report.errors.push_back({ "PAR_ARTIFACT_MISSING", "error", "ValidatingArtifact", "",
                                  errorMessage, 0 });
        ToolJobExecutor().Finish(job, report);
        return false;
    }
    if (!JobService().RecordArtifact(request.projectPath, job.id, request.outputJsonPath,
                                     "pnr_json", errorMessage)) {
        report.state = ToolJobState::Failed;
        report.summary = errorMessage;
        report.errors.push_back({ "PAR_ARTIFACT_UNSAFE", "error", "ValidatingArtifact", "",
                                  errorMessage, 0 });
        ToolJobExecutor().Finish(job, report);
        return false;
    }
    JobReport persisted;
    if (!JobService().LoadReport(request.projectPath, job.id, persisted, errorMessage)) {
        report.state = ToolJobState::Failed;
        report.summary = errorMessage;
        ToolJobExecutor().Finish(job, report);
        return false;
    }
    report = persisted;
    report.state = ToolJobState::Succeeded;
    report.summary = maxFrequencyMHz > 0.0
        ? wxString::Format("PnR completed; fmax = %.2f MHz.", maxFrequencyMHz)
        : wxString("PnR completed.");
    FinishJob(job, ToolJobState::Succeeded, report.exitCode, report.summary);
    return JobService().WriteReport(request.projectPath, job.id, report, errorMessage);
}
