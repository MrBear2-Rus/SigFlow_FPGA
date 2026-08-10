#define _WIN32_WINNT 0x0601
#define WINVER 0x0601

#include "NextpnrExecutor.h"

#include <wx/file.h>
#include <wx/dir.h>
#include <wx/filename.h>
#include <wx/log.h>

#include <algorithm>
#include <string>

namespace {

constexpr DWORD kPipeBufferSize = 64 * 1024;
constexpr DWORD kPollIntervalMs = 50;

wxString QuoteWindowsArgument(const wxString& value)
{
    if (value.IsEmpty()) {
        return "\"\"";
    }
    if (!value.Contains(' ') && !value.Contains('\t') && !value.Contains('"')) {
        return value;
    }

    wxString quoted = "\"";
    size_t backslashes = 0;
    for (const wxChar character : value) {
        if (character == '\\') {
            ++backslashes;
            continue;
        }
        if (character == '"') {
            quoted.Append('\\', backslashes * 2 + 1);
            quoted += '"';
            backslashes = 0;
            continue;
        }
        quoted.Append('\\', backslashes);
        backslashes = 0;
        quoted += character;
    }
    quoted.Append('\\', backslashes * 2);
    quoted += '"';
    return quoted;
}

wxString BuildCommandLine(const wxString& executable, const std::vector<wxString>& args)
{
    wxString commandLine = QuoteWindowsArgument(executable);
    for (const wxString& arg : args) {
        commandLine += " ";
        commandLine += QuoteWindowsArgument(arg);
    }
    return commandLine;
}

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
            check.message = "找不到 nextpnr 可执行文件，"
                            "请检查 sigflow.project -> fpga.nextpnr_path。"
                            "路径: " + executablePath;
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
        const wxString chipdbPath =
            shareDirectory + "\\himbaechel\\gowin\\chipdb-GW1N-9C.bin";
        NextpnrRuntimeCheck check;
        check.name = "chipdb-GW1N-9C.bin";
        check.path = chipdbPath;
        check.passed = FileExists(chipdbPath);
        if (!check.passed) {
            check.message = "芯片数据库文件缺失，"
                            "请确认 nextpnr runtime 是否解压完整。"
                            "路径: " + chipdbPath;
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
            check.message = "nextpnr share 目录不存在，"
                            "请确认 nextpnr runtime 是否解压完整。"
                            "路径: " + shareDirectory;
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
    Cleanup();
}

bool NextpnrExecutor::Execute(const wxString& executable,
                                   const std::vector<wxString>& args,
                                   const Config& config,
                                   OutputCallback onOutput,
                                   CompletionCallback onComplete)
{
    if (m_state.load() != State::Idle) {
        wxLogWarning("NextpnrExecutor: executor is not idle.");
        return false;
    }

    m_config = config;
    m_outputCallback = std::move(onOutput);
    m_completionCallback = std::move(onComplete);
    m_shouldTerminate = false;
    m_exitCode = -1;
    m_processId = 0;
    m_completionReason = CompletionReason::Success;
    {
        std::lock_guard<std::mutex> lock(m_logMutex);
        m_combinedLog.clear();
        m_logSize = 0;
    }

    if (!CreatePipes() || !CreateProcessAndJob(executable, args)) {
        m_completionReason = CompletionReason::LaunchFailed;
        Cleanup();
        return false;
    }

    if (m_hStdOutWrite) {
        CloseHandle(m_hStdOutWrite);
        m_hStdOutWrite = nullptr;
    }
    if (m_hStdErrWrite) {
        CloseHandle(m_hStdErrWrite);
        m_hStdErrWrite = nullptr;
    }

    m_startTime = std::chrono::steady_clock::now();
    m_state = State::Running;
    m_outputThread = std::thread(&NextpnrExecutor::OutputThreadFunc, this);
    return true;
}

void NextpnrExecutor::Cancel()
{
    if (m_state.load() != State::Running) {
        return;
    }
    m_shouldTerminate = true;
    KillProcessTree();
}

bool NextpnrExecutor::CreatePipes()
{
    SECURITY_ATTRIBUTES attributes = {};
    attributes.nLength = sizeof(attributes);
    attributes.bInheritHandle = TRUE;

    if (!CreatePipe(&m_hStdOutRead, &m_hStdOutWrite, &attributes, kPipeBufferSize) ||
        !SetHandleInformation(m_hStdOutRead, HANDLE_FLAG_INHERIT, 0)) {
        wxLogError("NextpnrExecutor: unable to create stdout pipe (%lu).", GetLastError());
        return false;
    }
    if (!CreatePipe(&m_hStdErrRead, &m_hStdErrWrite, &attributes, kPipeBufferSize) ||
        !SetHandleInformation(m_hStdErrRead, HANDLE_FLAG_INHERIT, 0)) {
        wxLogError("NextpnrExecutor: unable to create stderr pipe (%lu).", GetLastError());
        return false;
    }
    return true;
}

bool NextpnrExecutor::CreateProcessAndJob(const wxString& executable,
                                               const std::vector<wxString>& args)
{
    STARTUPINFOW startupInfo = {};
    startupInfo.cb = sizeof(startupInfo);
    startupInfo.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    startupInfo.wShowWindow = SW_HIDE;
    startupInfo.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startupInfo.hStdOutput = m_hStdOutWrite;
    startupInfo.hStdError = m_hStdErrWrite;

    PROCESS_INFORMATION processInfo = {};
    const wxString commandLine = BuildCommandLine(executable, args);
    std::wstring writableCommandLine = commandLine.ToStdWstring();
    std::wstring workingDirectory = m_config.workingDirectory.ToStdWstring();

    if (!CreateProcessW(executable.wc_str(), writableCommandLine.data(), nullptr, nullptr, TRUE,
                        CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT, nullptr,
                        workingDirectory.empty() ? nullptr : workingDirectory.c_str(),
                        &startupInfo, &processInfo)) {
        wxLogError("NextpnrExecutor: CreateProcessW failed (%lu).", GetLastError());
        return false;
    }

    m_hProcess = processInfo.hProcess;
    m_processId = processInfo.dwProcessId;
    m_hJobObject = CreateJobObjectW(nullptr, nullptr);
    if (!m_hJobObject) {
        wxLogError("NextpnrExecutor: CreateJobObjectW failed (%lu).", GetLastError());
        TerminateProcess(m_hProcess, 1);
        CloseHandle(processInfo.hThread);
        return false;
    }

    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (m_config.memoryLimitBytes > 0) {
        limits.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_PROCESS_MEMORY;
        limits.ProcessMemoryLimit = static_cast<SIZE_T>(m_config.memoryLimitBytes);
    }
    if (!SetInformationJobObject(m_hJobObject, JobObjectExtendedLimitInformation,
                                 &limits, sizeof(limits))) {
        wxLogError("NextpnrExecutor: SetInformationJobObject failed (%lu).", GetLastError());
        TerminateProcess(m_hProcess, 1);
        CloseHandle(processInfo.hThread);
        return false;
    }
    if (!AssignProcessToJobObject(m_hJobObject, m_hProcess)) {
        wxLogError("NextpnrExecutor: AssignProcessToJobObject failed (%lu).", GetLastError());
        TerminateProcess(m_hProcess, 1);
        CloseHandle(processInfo.hThread);
        return false;
    }
    if (ResumeThread(processInfo.hThread) == static_cast<DWORD>(-1)) {
        wxLogError("NextpnrExecutor: ResumeThread failed (%lu).", GetLastError());
        TerminateJobObject(m_hJobObject, 1);
        CloseHandle(processInfo.hThread);
        return false;
    }

    CloseHandle(processInfo.hThread);
    return true;
}

void NextpnrExecutor::KillProcessTree()
{
    if (m_hJobObject) {
        TerminateJobObject(m_hJobObject, 1);
    } else if (m_hProcess) {
        TerminateProcess(m_hProcess, 1);
    }
}

void NextpnrExecutor::DrainPipe(HANDLE pipe, OutputStream stream, bool drainAll)
{
    if (!pipe) {
        return;
    }

    char buffer[4096];
    do {
        DWORD available = 0;
        if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr) || available == 0) {
            break;
        }
        DWORD bytesRead = 0;
        const DWORD toRead = (std::min)(available, static_cast<DWORD>(sizeof(buffer)));
        if (!ReadFile(pipe, buffer, toRead, &bytesRead, nullptr) || bytesRead == 0) {
            break;
        }

        const wxString text = wxString::FromUTF8(buffer, bytesRead);
        {
            std::lock_guard<std::mutex> lock(m_logMutex);
            if (m_config.logSizeLimit == 0) {
                m_combinedLog += text;
                m_logSize += bytesRead;
            } else {
                m_combinedLog += text;
                m_logSize += bytesRead;
                while (!m_combinedLog.IsEmpty() && m_logSize > m_config.logSizeLimit) {
                    const wxString firstCharacter = m_combinedLog.Left(1);
                    const wxScopedCharBuffer firstCharacterUtf8 = firstCharacter.ToUTF8();
                    m_logSize -= (std::min)(m_logSize, firstCharacterUtf8.length());
                    m_combinedLog.Remove(0, 1);
                }
            }
        }
        if (m_outputCallback) {
            m_outputCallback(stream, text);
        }
    } while (drainAll);
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
        wxLogWarning("NextpnrExecutor: unable to write combined log: %s", m_config.combinedLogPath);
        return false;
    }
    const wxScopedCharBuffer utf8 = combinedLog.ToUTF8();
    const char* data = utf8.data();
    const size_t length = data ? utf8.length() : 0;
    const bool written = length == 0 ||
        logFile.Write(data, length) == static_cast<wxFileOffset>(length);
    if (!written) {
        wxLogWarning("NextpnrExecutor: unable to finish combined log: %s", m_config.combinedLogPath);
    }
    logFile.Close();
    return written;
}

void NextpnrExecutor::OutputThreadFunc()
{
    bool processEnded = false;
    while (!processEnded) {
        if (WaitForSingleObject(m_hProcess, 0) == WAIT_OBJECT_0) {
            processEnded = true;
        } else if (m_config.timeLimitSec > 0 &&
                   std::chrono::steady_clock::now() - m_startTime >=
                       std::chrono::seconds(m_config.timeLimitSec)) {
            m_completionReason = CompletionReason::TimedOut;
            m_shouldTerminate = true;
            KillProcessTree();
        } else if (m_shouldTerminate.load()) {
            m_completionReason = CompletionReason::Cancelled;
            KillProcessTree();
        }

        DrainPipe(m_hStdOutRead, OutputStream::StdOut, false);
        DrainPipe(m_hStdErrRead, OutputStream::StdErr, false);
        if (!processEnded) {
            Sleep(kPollIntervalMs);
        }
    }

    DrainPipe(m_hStdOutRead, OutputStream::StdOut, true);
    DrainPipe(m_hStdErrRead, OutputStream::StdErr, true);

    DWORD exitCode = 1;
    if (GetExitCodeProcess(m_hProcess, &exitCode)) {
        m_exitCode = static_cast<int>(exitCode);
    }
    if (m_completionReason == CompletionReason::Success && m_shouldTerminate) {
        m_completionReason = CompletionReason::Cancelled;
    } else if (m_completionReason == CompletionReason::Success && m_exitCode != 0) {
        m_completionReason = CompletionReason::NonZeroExit;
    }

    const bool combinedLogWritten = WriteCombinedLog();

    Result result;
    result.reason = m_completionReason.load();
    result.exitCode = m_exitCode.load();
    result.processId = m_processId.load();
    result.combinedLogWritten = combinedLogWritten;
    {
        std::lock_guard<std::mutex> lock(m_logMutex);
        result.combinedLog = m_combinedLog;
    }
    m_state = State::Completed;
    if (m_completionCallback) {
        m_completionCallback(result);
    }
}

void NextpnrExecutor::CloseHandles()
{
    if (m_hStdOutRead) { CloseHandle(m_hStdOutRead); m_hStdOutRead = nullptr; }
    if (m_hStdOutWrite) { CloseHandle(m_hStdOutWrite); m_hStdOutWrite = nullptr; }
    if (m_hStdErrRead) { CloseHandle(m_hStdErrRead); m_hStdErrRead = nullptr; }
    if (m_hStdErrWrite) { CloseHandle(m_hStdErrWrite); m_hStdErrWrite = nullptr; }
    if (m_hProcess) { CloseHandle(m_hProcess); m_hProcess = nullptr; }
    if (m_hJobObject) { CloseHandle(m_hJobObject); m_hJobObject = nullptr; }
}

void NextpnrExecutor::Cleanup()
{
    if (m_outputThread.joinable()) {
        m_outputThread.join();
    }
    CloseHandles();
    m_state = State::Idle;
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
    result.pnrJsonPath = m_request.outputDirectory + "\\" +
                         m_request.topModule + ".pnr.json";

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
        "--write", req.outputDirectory + "\\" + req.topModule + ".pnr.json",
        "--device", req.deviceName,
        "--vopt", "family=" + req.familyName,
        "--vopt", "cst=" + req.configuredCstPath,
    };
}

bool NextpnrExecutor::ValidateArtifact(const wxString& pnrJsonPath) {
    return FileExists(pnrJsonPath);
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

    const wxString reportDir = m_request.projectPath + "\\nextpnr";
    EnsureDirectory(reportDir);
    result.analysisJsonPath =
        reportDir + "\\" + m_request.topModule + ".analysis.json";
    if (!m_report.SaveReport(record, result.analysisJsonPath)) {
        result.terminalSummary += "\n[警告] 无法保存分析报告: " + result.analysisJsonPath + "\n";
    }
}
