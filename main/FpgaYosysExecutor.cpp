#include "FpgaYosysExecutor.h"

#include <wx/file.h>

#include "jobs/PlatformProcess.h"
#include "platform/Log.h"

#include <algorithm>
#include <cstdint>

namespace {
// 未显式配置超时时的兜底上限。
// 必须有限：0 会被平台层理解为"无超时"（Windows INFINITE / POSIX 无超时轮询），
// 工具一旦卡死，任务就会永远停在 Running，取消也无效。
constexpr int kDefaultToolTimeoutSec = 600;
} // namespace


YosysExecutor::YosysExecutor() = default;

YosysExecutor::~YosysExecutor()
{
    Cancel();
    if (m_worker.joinable()) {
        m_worker.join();
    }
}

bool YosysExecutor::Execute(const wxString& executable,
                            const std::vector<wxString>& args,
                            const Config& config,
                            OutputCallback onOutput,
                            CompletionCallback onComplete)
{
    if (m_state.load() != State::Idle) {
        SIGFLOW_LOG("YosysExecutor: executor is not idle.");
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
        // timeLimitSec <= 0 表示未配置：回落到有限上限，避免工具卡死时任务永不结束。
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
            SIGFLOW_LOG(wxString("YosysExecutor: ") + processResult.errorMessage);
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

void YosysExecutor::Cancel()
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

bool YosysExecutor::WriteCombinedLog()
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
        SIGFLOW_LOG("YosysExecutor: unable to write combined log: " + m_config.combinedLogPath);
        return false;
    }
    const wxScopedCharBuffer utf8 = combinedLog.ToUTF8();
    const char* data = utf8.data();
    const size_t length = data ? utf8.length() : 0;
    const bool written = length == 0 ||
        logFile.Write(data, length) == static_cast<wxFileOffset>(length);
    if (!written) {
        SIGFLOW_LOG("YosysExecutor: unable to finish combined log: " + m_config.combinedLogPath);
    }
    logFile.Close();
    return written;
}
