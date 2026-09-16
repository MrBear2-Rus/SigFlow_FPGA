#include "NextpnrExecutor.h"

#include <wx/file.h>
#include <wx/dir.h>
#include <wx/filename.h>

#include "../jobs/PlatformProcess.h"
#include "../platform/Log.h"
#include "../platform/PlatformPaths.h"

using sigflow::platform::JoinPath;

#include <algorithm>
#include <cstdint>

namespace {

// 未显式配置超时时的兜底上限。
// 必须有限：0 会被平台层理解为"无超时"（Windows INFINITE / POSIX 无超时轮询），
// 工具一旦卡死，任务就会永远停在 Running，取消也无效。
constexpr int kDefaultToolTimeoutSec = 600;

bool FileExists(const wxString& path) {
    return wxFile::Exists(path);
}

bool EnsureDirectory(const wxString& path) {
    if (wxDir::Exists(path)) return true;
    return wxFileName::Mkdir(path, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL);
}

} // namespace

// ---------------------------------------------------------------------------
// 运行环境预检
// ---------------------------------------------------------------------------

NextpnrRuntimeReport ValidateNextpnrRuntime(const wxString& executablePath,
                                             const wxString& shareDirectory) {
    NextpnrRuntimeReport report;
    report.executablePath = executablePath;
    report.shareDirectory = shareDirectory;
    report.valid = true;

    {
        NextpnrRuntimeCheck check;
        check.name = "nextpnr-himbaechel.exe";
        check.path = executablePath;
        check.passed = FileExists(executablePath);
        if (!check.passed) {
            check.message = wxT("找不到 nextpnr 可执行文件，")
                            wxT("请检查 sigflow.project -> fpga.nextpnr_path。")
                            wxT("路径: ") + executablePath;
            report.valid = false;
        } else {
            check.message = "OK";
        }
        report.checks.push_back(check);
    }

    {
        // TODO: chipdb 文件名目前硬编码为 Tang Nano 9K (GW1N-9C)。
        // 多设备支持时应从 targetProfile 动态派生，例如：
        //   chipdb- + familyName + ".bin"
        const wxString chipdbPath = JoinPath(
            JoinPath(JoinPath(shareDirectory, "himbaechel"), "gowin"),
            "chipdb-GW1N-9C.bin");
        NextpnrRuntimeCheck check;
        check.name = "chipdb-GW1N-9C.bin";
        check.path = chipdbPath;
        check.passed = FileExists(chipdbPath);
        if (!check.passed) {
            check.message = wxT("芯片数据库文件缺失，")
                            wxT("请确认 nextpnr runtime 是否解压完整。")
                            wxT("路径: ") + chipdbPath;
            report.valid = false;
        } else {
            check.message = "OK";
        }
        report.checks.push_back(check);
    }

    {
        NextpnrRuntimeCheck check;
        check.name = "share directory";
        check.path = shareDirectory;
        check.passed = wxDir::Exists(shareDirectory);
        if (!check.passed) {
            check.message = wxT("nextpnr share 目录不存在，")
                            wxT("请确认 nextpnr runtime 是否解压完整。")
                            wxT("路径: ") + shareDirectory;
            report.valid = false;
        } else {
            check.message = "OK";
        }
        report.checks.push_back(check);
    }

    return report;
}

wxString NextpnrRuntimeReport::FormatForTerminal() const {
    wxString out;
    out += "=== Nextpnr Runtime Check ===\n";
    out += "Executable: " + executablePath + "\n";
    out += "Share Dir:  " + shareDirectory + "\n\n";

    for (const auto& check : checks) {
        out += wxString::Format("  [%s] %s\n",
            check.passed ? "PASS" : "FAIL", check.name);
        if (!check.passed) {
            out += "    -> " + check.message + "\n";
        }
    }

    out += "\n";
    out += valid ? "All checks passed.\n" : "Some checks FAILED.\n";
    return out;
}

// ---------------------------------------------------------------------------
// NextpnrExecutor — 进程管理（仿照 YosysExecutor）
// ---------------------------------------------------------------------------

NextpnrExecutor::NextpnrExecutor() = default;

NextpnrExecutor::~NextpnrExecutor()
{
    Cancel();
    if (m_worker.joinable()) {
        m_worker.join();
    }
}

bool NextpnrExecutor::Execute(const wxString& executable,
                                   const std::vector<wxString>& args,
                                   const Config& config,
                                   OutputCallback onOutput,
                                   CompletionCallback onComplete)
{
    if (m_state.load() != State::Idle) {
        SIGFLOW_LOG("NextpnrExecutor: executor is not idle.");
        return false;
    }

    m_config = config;
    m_outputCallback = std::move(onOutput);
    m_completionCallback = std::move(onComplete);
    m_shouldTerminate = false;
    m_exitCode = -1;
    m_completionReason = CompletionReason::Success;
    {
        std::lock_guard<std::mutex> lock(m_handleMutex);
        m_handle = nullptr;
    }
    {
        std::lock_guard<std::mutex> lock(m_logMutex);
        m_combinedLog.clear();
        m_logSize = 0;
    }
    m_state = State::Running;

    m_worker = std::thread([this, executable, args, config]() {
        PlatformProcessRequest request;
        request.executable = executable;
        request.arguments = args;
        request.workingDirectory = config.workingDirectory;
        // timeLimitSec <= 0 表示"未配置"。不能直接把 0 传给平台层：
        // 那意味着**完全没有超时**（Windows 是 WaitForSingleObject(INFINITE)，
        // POSIX 是无超时轮询），nextpnr 一旦卡死任务就会永远停在 Running。
        request.timeoutSeconds = config.timeLimitSec > 0
            ? config.timeLimitSec : kDefaultToolTimeoutSec;
        request.maxOutputBytes = 0;
        request.memoryLimitBytes = static_cast<std::uint64_t>(config.memoryLimitBytes);
        request.onStarted = [this](void* handle) {
            bool terminateNow = false;
            {
                std::lock_guard<std::mutex> lock(m_handleMutex);
                m_handle = handle;
                terminateNow = m_shouldTerminate.load();
            }
            if (terminateNow) {
                PlatformProcess::Terminate(handle);
            }
        };
        request.onFinished = [this]() {
            std::lock_guard<std::mutex> lock(m_handleMutex);
            m_handle = nullptr;
        };

        const PlatformOutputCallback sink =
            [this](const wxString& chunk, bool isError) {
                if (chunk.IsEmpty()) {
                    return;
                }
                {
                    std::lock_guard<std::mutex> lock(m_logMutex);
                    m_combinedLog += chunk;
                    m_logSize += chunk.ToUTF8().length();
                    if (m_config.logSizeLimit > 0) {
                        while (!m_combinedLog.IsEmpty() &&
                               m_logSize > m_config.logSizeLimit) {
                            const wxString firstCharacter = m_combinedLog.Left(1);
                            const wxScopedCharBuffer firstCharacterUtf8 =
                                firstCharacter.ToUTF8();
                            m_logSize -= (std::min)(m_logSize,
                                                    firstCharacterUtf8.length());
                            m_combinedLog.Remove(0, 1);
                        }
                    }
                }
                if (m_outputCallback) {
                    m_outputCallback(isError ? OutputStream::StdErr : OutputStream::StdOut, chunk);
                }
            };

        const PlatformProcessResult processResult = PlatformProcess::Run(request, sink);
        {
            std::lock_guard<std::mutex> lock(m_handleMutex);
            m_handle = nullptr;
        }

        if (!processResult.started && !processResult.errorMessage.IsEmpty()) {
            SIGFLOW_LOG(wxString("NextpnrExecutor: ") + processResult.errorMessage);
        }

        CompletionReason reason = CompletionReason::Success;
        if (m_shouldTerminate.load()) {
            reason = CompletionReason::Cancelled;
        } else if (processResult.timedOut) {
            reason = CompletionReason::TimedOut;
        } else if (!processResult.started) {
            reason = CompletionReason::LaunchFailed;
        } else if (processResult.exitCode != 0) {
            reason = CompletionReason::NonZeroExit;
        }
        m_completionReason.store(reason);
        m_exitCode.store(processResult.exitCode);

        const bool combinedLogWritten = WriteCombinedLog();

        Result result;
        result.reason = reason;
        result.exitCode = processResult.exitCode;
        result.processId = 0;
        result.combinedLogWritten = combinedLogWritten;
        {
            std::lock_guard<std::mutex> lock(m_logMutex);
            result.combinedLog = m_combinedLog;
        }
        m_state = State::Completed;
        if (m_completionCallback) {
            m_completionCallback(result);
        }
    });

    return true;
}

void NextpnrExecutor::Cancel()
{
    if (m_state.load() != State::Running) {
        return;
    }
    m_shouldTerminate = true;
    void* handle = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_handleMutex);
        handle = m_handle;
    }
    if (handle) {
        PlatformProcess::Terminate(handle);
    }
}

bool NextpnrExecutor::WriteCombinedLog()
{
    if (m_config.combinedLogPath.IsEmpty()) {
        return true;
    }

    wxString combinedLog;
    {
        std::lock_guard<std::mutex> lock(m_logMutex);
        combinedLog = m_combinedLog;
    }

    wxFile logFile(m_config.combinedLogPath, wxFile::write);
    if (!logFile.IsOpened()) {
        SIGFLOW_LOG("NextpnrExecutor: unable to write combined log: " + m_config.combinedLogPath);
        return false;
    }
    const wxScopedCharBuffer utf8 = combinedLog.ToUTF8();
    const char* data = utf8.data();
    const size_t length = data ? utf8.length() : 0;
    const bool written = length == 0 ||
        logFile.Write(data, length) == static_cast<wxFileOffset>(length);
    if (!written) {
        SIGFLOW_LOG("NextpnrExecutor: unable to finish combined log: " + m_config.combinedLogPath);
    }
    logFile.Close();
    return written;
}

// ---------------------------------------------------------------------------
// NextpnrExecutor — nextpnr 业务逻辑
// ---------------------------------------------------------------------------

bool NextpnrExecutor::Prepare(NextpnrExecuteRequest& req, wxString& errorMsg) {
    m_request = req;
    m_jobStartTime = wxDateTime::Now();
    m_arguments.clear();

    if (!ValidateInputs(m_request, errorMsg)) {
        return false;
    }

    m_arguments = BuildArguments(m_request);
    req.configuredCstPath = m_request.configuredCstPath;
    return true;
}

NextpnrJobResult NextpnrExecutor::Finalize(
    int exitCode, const wxString& combinedLog) {

    NextpnrJobResult result;
    result.exitCode = exitCode;
    result.startTime = m_jobStartTime;
    result.pnrJsonPath = JoinPath(m_request.outputDirectory,
                                  m_request.topModule + ".pnr.json");

    RunAnalysis(combinedLog, result);

    if (exitCode == 0) {
        wxString artifactPath;
        if (ValidateArtifact(result.pnrJsonPath)) {
            result.succeeded = true;
        } else {
            result.succeeded = false;
            result.terminalSummary +=
                "\n[错误] nextpnr 成功退出但未找到 .pnr.json 产物。\n"
                "期望路径: " + result.pnrJsonPath + "\n";
        }
    } else {
        result.succeeded = false;
    }

    result.endTime = wxDateTime::Now();
    result.elapsedSec =
        (result.endTime - m_jobStartTime).GetSeconds().ToDouble();

    return result;
}

bool NextpnrExecutor::ValidateInputs(NextpnrExecuteRequest& req,
                                          wxString& errorMsg) {
    if (!FileExists(req.jsonPath)) {
        errorMsg = "找不到 JSON 网表:\n" + req.jsonPath +
                   "\n请先运行 FPGA > Synthesis 综合。\n";
        return false;
    }

    const wxString resolved =
        CstValidator::AutoResolveCst(req.projectPath, req.configuredCstPath);
    if (resolved.IsEmpty() || !FileExists(resolved)) {
        errorMsg = "找不到 CST 约束文件。\n"
                   "搜索路径: " + req.configuredCstPath + "\n"
                   "请打开 FPGA > Pin Constraints 绑定端口并生成 CST。\n";
        return false;
    }
    req.configuredCstPath = resolved;

    const CstValidationResult cstResult =
        CstValidator::Validate(req.configuredCstPath);
    if (!cstResult.valid) {
        errorMsg = "CST 校验失败:\n" + cstResult.errorSummary;
        return false;
    }

    return true;
}

std::vector<wxString> NextpnrExecutor::BuildArguments(
    const NextpnrExecuteRequest& req) {
    return {
        "--json", req.jsonPath,
        "--write", JoinPath(req.outputDirectory, req.topModule + ".pnr.json"),
        "--device", req.deviceName,
        "--vopt", "family=" + req.familyName,
        "--vopt", "cst=" + req.configuredCstPath,
    };
}

bool NextpnrExecutor::ValidateArtifact(const wxString& pnrJsonPath) {
    // 只判"文件存在"是不够的：nextpnr 崩溃/磁盘写满时会留下 0 字节或截断的
    // .pnr.json，却被判定为成功并继续喂给 gowin_pack。
    // 这里额外要求文件非空（ToolJobs 对同类产物就是这么校验的）。
    if (!FileExists(pnrJsonPath)) {
        return false;
    }
    const wxFileName info(pnrJsonPath);
    if (!info.FileExists()) {
        return false;
    }
    wxFile file(pnrJsonPath, wxFile::read);
    if (!file.IsOpened()) {
        return false;
    }
    const bool ok = file.Length() > 0;
    file.Close();
    return ok;
}

void NextpnrExecutor::RunAnalysis(const wxString& combinedLog,
                                       NextpnrJobResult& result) {
    NextpnrRunRecord record;
    record.executablePath = m_request.executablePath;
    record.workingDir = m_request.outputDirectory;

    const bool parseOk = m_logParser.Parse(combinedLog, "", record);
    if (parseOk) {
        result.terminalSummary = m_report.FormatSummary(record);
    } else {
        result.terminalSummary = "[nextpnr] 日志分析未产生有效结果。\n";
    }

    const wxString reportDir = JoinPath(m_request.projectPath, "nextpnr");
    EnsureDirectory(reportDir);
    result.analysisJsonPath =
        JoinPath(reportDir, m_request.topModule + ".analysis.json");
    if (!m_report.SaveReport(record, result.analysisJsonPath)) {
        result.terminalSummary += "\n[警告] 无法保存分析报告: " + result.analysisJsonPath + "\n";
    }
}
