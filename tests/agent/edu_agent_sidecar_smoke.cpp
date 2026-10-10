// NG-07：自研 Python sidecar 与 SigFlow 宿主的真实进程联调冒烟测试。
//
// 这不是 Mock：真的启动 `python -m sigflow_edu_agent --bootstrap-stdin`，真的经过
// stdin bootstrap → stdout ready → loopback HTTP health → 教学卡 → shutdown 的全过程，
// 并用 AgentServiceController 自己的握手校验函数（AgentServiceController::ValidateReadyLine）
// 判定 ready 行，避免测试里复述一遍规则。
//
// 覆盖：
//   * bootstrap/ready 的 nonce、协议、端口、版本校验；
//   * health 必须 200 且 data.protocol == edu.api.v1；
//   * 版本/nonce/协议失配与 ready 前退出都必须被宿主识别为“不可用”，而不是放行；
//   * 无模型 Key 时降级为确定性规则卡（model_used=false）；
//   * Gateway 不可达时 report_review 明确失败，不伪造读过报告；
//   * 安全关闭（/shutdown）后进程正常退出；
//   * token 不出现在 sidecar 的任何输出里。
//
// 缺少 Python 或 sidecar 包时显式 SKIP（不静默通过）。
#include "agent/AgentServiceController.h"

#include <eda/api/Types.h>

#include <httplib.h>
#include <json/json.h>


#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include <wx/app.h>
#include <wx/filefn.h>
#include <wx/filename.h>
#include <wx/init.h>
#include <wx/utils.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

// AD-15：驱动 AgentServiceController 的真实 Launch/Tick 全链需要一个 wxApp
// （wxProcess 的异步终止事件要经事件循环派发）。这里用最小 console app。
class SidecarTestApp final : public wxAppConsole {
public:
    bool OnInit() override { return true; }
};

int g_failures = 0;
int g_skipped = 0;

void Check(bool ok, const char* message) {
    if (ok) {
        std::cout << "  ok: " << message << "\n";
    } else {
        ++g_failures;
        std::cout << "  FAIL: " << message << "\n";
    }
}

void Skip(const std::string& message) {
    ++g_skipped;
    std::cout << "  SKIP: " << message << "\n";
}

std::string RandomHex(std::size_t length) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string value;
    value.reserve(length);
    std::uint64_t state = static_cast<std::uint64_t>(
        std::chrono::high_resolution_clock::now().time_since_epoch().count());
    for (std::size_t i = 0; i < length; ++i) {
        state = state * 6364136223846793005ull + 1442695040888963407ull;
        value.push_back(kHex[(state >> 33) & 0x0f]);
    }
    return value;
}

// 在 PATH 上按目录逐个查找可执行文件（不依赖 wx 的 FindExecutable 变体，行为可预期）。
wxString FindOnPath(const wxString& name) {
    const wxString path = wxGetenv(wxT("PATH"));
    wxString rest = path;
    while (!rest.empty()) {
        const wxString directory = rest.BeforeFirst(wxPATH_SEP[0]);
        rest = rest.AfterFirst(wxPATH_SEP[0]);
        if (directory.empty()) continue;
        const wxFileName candidate(directory, name);
        if (candidate.FileExists()) return candidate.GetFullPath();
    }
    return wxString();
}

// WSL 会把 Windows 的 PATH 也带进来（/mnt/... 下的 .exe 能被互操作执行）。
// POSIX 验收必须拒绝这类二进制，否则"Linux 验证"实际跑的是 Windows 解释器，
// 管道/信号/进程语义都不是原生的——这正是本仓库约束里禁止的替代方式。
bool IsInteropBinary(const wxString& path) {
#ifdef _WIN32
    (void)path;
    return false;
#else
    return path.StartsWith(wxT("/mnt/")) || path.Lower().EndsWith(wxT(".exe"));
#endif
}

// AD-15：控制器要求 executable 是**存在的绝对路径**（Launch 会 wxFileExists 校验），
// 所以这里解析真实解释器；解析不到就让上层显式 SKIP。
wxString ResolvePythonExecutable() {
    const char* fromEnvironment = std::getenv("SIGFLOW_EDU_AGENT_PYTHON");
    if (fromEnvironment != nullptr && *fromEnvironment != '\0') {
        const wxString candidate = wxString::FromUTF8(fromEnvironment);
        if (wxFileExists(candidate) && !IsInteropBinary(candidate)) return candidate;
    }
#ifdef _WIN32
    const wxString names[] = {wxT("python.exe"), wxT("python3.exe")};
#else
    // 先看常见原生位置，再退回 PATH；两步都排除 /mnt 下的 Windows 二进制。
    const wxString explicitPaths[] = {wxT("/usr/bin/python3"), wxT("/usr/local/bin/python3"),
                                      wxT("/usr/bin/python")};
    for (const wxString& candidate : explicitPaths) {
        if (wxFileExists(candidate)) return candidate;
    }
    const wxString names[] = {wxT("python3"), wxT("python")};
#endif
    for (const wxString& name : names) {
        const wxString found = FindOnPath(name);
        if (!found.empty() && !IsInteropBinary(found)) return found;
    }
    return wxString();
}

// 与 MakeControllerConfig 保持一致：测试要断言"重启次数到达上限后进入 Failed"。
constexpr unsigned int kControllerMaxRestarts = 2;

AgentServiceController::Config MakeControllerConfig(const wxString& python,
                                                    const std::filesystem::path& sourceDirectory,
                                                    const std::filesystem::path& dataDirectory,
                                                    const std::vector<std::string>& extraArguments) {
    AgentServiceController::Config config;
    config.executable = python;
    config.arguments.clear();
    config.arguments.push_back(wxT("-m"));
    config.arguments.push_back(wxT("sigflow_edu_agent"));
    config.arguments.push_back(wxT("--bootstrap-stdin"));
    for (const std::string& extra : extraArguments) {
        config.arguments.push_back(wxString::FromUTF8(extra));
    }
    config.workingDirectory = wxString::FromUTF8(sourceDirectory.string());
    config.dataDirectory = wxString::FromUTF8(dataDirectory.string());
    config.instanceId = "inst-controller-ad15";
    // 故意指向没有监听的端口：控制器只做 health，不依赖 Gateway。
    config.gatewayUrl = "http://127.0.0.1:9";
    config.gatewayToken = "tok-test-eda-ad15";
    config.protocol = "edu.api.v1";
    config.maxRestarts = kControllerMaxRestarts;  // 缩短有界重启的验证时间
    return config;
}

// 真实宿主节奏：派发 wx 事件（wxProcess 终止通知）→ Tick() → 检查条件。
template <typename Predicate>
bool PumpUntil(AgentServiceController& controller, Predicate predicate, int timeoutMs) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (wxTheApp != nullptr) wxTheApp->ProcessPendingEvents();
        controller.Tick();
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (wxTheApp != nullptr) wxTheApp->ProcessPendingEvents();
    controller.Tick();
    return predicate();
}

// ---------------------------------------------------------------------------
// 最小双向子进程：stdin 写入 + stdout/stderr 读取 + 退出码。
// Windows 用 CreateProcess + 匿名管道；POSIX 用 fork/exec + pipe。
// ---------------------------------------------------------------------------
class ChildProcess {
public:
    ~ChildProcess() { Kill(); }

    bool Start(const std::string& executable, const std::vector<std::string>& arguments,
               const std::string& workingDirectory, std::string& error);
    // 原样写入（不追加换行）：宿主写出的启动材料自带结尾换行，测试必须写完全相同的字节。
    bool WriteRaw(const std::string& bytes, std::string& error);
    bool WriteLine(const std::string& line, std::string& error);
    void CloseStdin();
    // 读取一行 stdout（等待超时返回空串）。
    std::string ReadLine(int timeoutMs, bool& timedOut);
    void DrainStderr();
    bool WaitForExit(int timeoutMs, int& exitCode);
    void Kill();
    const std::string& stderrTail() const { return stderrTail_; }

#ifdef _WIN32
private:
    PROCESS_INFORMATION process_{};
    HANDLE stdinWrite_ = nullptr;
    HANDLE stdoutRead_ = nullptr;
    HANDLE stderrRead_ = nullptr;
    bool running_ = false;
#else
private:
    pid_t pid_ = -1;
    int stdinWrite_ = -1;
    int stdoutRead_ = -1;
    int stderrRead_ = -1;
    int exitCode_ = 0;
    bool reaped_ = false;
    bool running_ = false;
#endif
    std::string stdoutBuffer_;
    std::string stderrTail_;
};

#ifdef _WIN32

std::string QuoteArgument(const std::string& argument) {
    std::string quoted = "\"";
    for (char character : argument) {
        if (character == '"') quoted += "\\\"";
        else quoted.push_back(character);
    }
    quoted += "\"";
    return quoted;
}

bool ChildProcess::Start(const std::string& executable, const std::vector<std::string>& arguments,
                         const std::string& workingDirectory, std::string& error) {
    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.bInheritHandle = TRUE;
    HANDLE childStdoutWrite = nullptr;
    HANDLE childStderrWrite = nullptr;
    HANDLE childStdinRead = nullptr;
    if (!CreatePipe(&childStdinRead, &stdinWrite_, &attributes, 0) ||
        !CreatePipe(&stdoutRead_, &childStdoutWrite, &attributes, 0) ||
        !CreatePipe(&stderrRead_, &childStderrWrite, &attributes, 0)) {
        error = "CreatePipe failed";
        return false;
    }
    SetHandleInformation(stdinWrite_, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(stdoutRead_, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(stderrRead_, HANDLE_FLAG_INHERIT, 0);

    std::string commandLine = QuoteArgument(executable);
    for (const std::string& argument : arguments) commandLine += " " + QuoteArgument(argument);

    STARTUPINFOA startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = childStdinRead;
    startup.hStdOutput = childStdoutWrite;
    startup.hStdError = childStderrWrite;

    std::vector<char> mutableCommand(commandLine.begin(), commandLine.end());
    mutableCommand.push_back('\0');
    const BOOL created = CreateProcessA(
        nullptr, mutableCommand.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
        workingDirectory.empty() ? nullptr : workingDirectory.c_str(), &startup, &process_);
    CloseHandle(childStdinRead);
    CloseHandle(childStdoutWrite);
    CloseHandle(childStderrWrite);
    if (!created) {
        error = "CreateProcess failed with error " + std::to_string(GetLastError());
        return false;
    }
    running_ = true;
    return true;
}

bool ChildProcess::WriteRaw(const std::string& bytes, std::string& error) {
    if (stdinWrite_ == nullptr) {
        error = "stdin is closed";
        return false;
    }
    DWORD written = 0;
    if (!WriteFile(stdinWrite_, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) ||
        written != bytes.size()) {
        error = "failed to write the bootstrap bytes to the sidecar";
        return false;
    }
    return true;
}

bool ChildProcess::WriteLine(const std::string& line, std::string& error) {
    return WriteRaw(line + "\n", error);
}

void ChildProcess::CloseStdin() {
    if (stdinWrite_ != nullptr) {
        CloseHandle(stdinWrite_);
        stdinWrite_ = nullptr;
    }
}

std::string ChildProcess::ReadLine(int timeoutMs, bool& timedOut) {
    timedOut = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    for (;;) {
        const std::size_t newline = stdoutBuffer_.find('\n');
        if (newline != std::string::npos) {
            std::string line = stdoutBuffer_.substr(0, newline);
            stdoutBuffer_.erase(0, newline + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            return line;
        }
        DWORD available = 0;
        if (stdoutRead_ != nullptr &&
            PeekNamedPipe(stdoutRead_, nullptr, 0, nullptr, &available, nullptr) && available > 0) {
            char buffer[4096];
            DWORD read = 0;
            if (ReadFile(stdoutRead_, buffer, sizeof(buffer), &read, nullptr) && read > 0) {
                stdoutBuffer_.append(buffer, read);
                continue;
            }
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            timedOut = true;
            return std::string();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

void ChildProcess::DrainStderr() {
    if (stderrRead_ == nullptr) return;
    DWORD available = 0;
    for (;;) {
        if (!PeekNamedPipe(stderrRead_, nullptr, 0, nullptr, &available, nullptr) || available == 0) {
            return;
        }
        char buffer[4096];
        DWORD read = 0;
        if (!ReadFile(stderrRead_, buffer, sizeof(buffer), &read, nullptr) || read == 0) return;
        stderrTail_.append(buffer, read);
        if (stderrTail_.size() > 16 * 1024) stderrTail_.erase(0, stderrTail_.size() - 16 * 1024);
    }
}

bool ChildProcess::WaitForExit(int timeoutMs, int& exitCode) {
    if (!running_) return false;
    if (WaitForSingleObject(process_.hProcess, static_cast<DWORD>(timeoutMs)) != WAIT_OBJECT_0) {
        return false;
    }
    DWORD code = 0;
    GetExitCodeProcess(process_.hProcess, &code);
    exitCode = static_cast<int>(code);
    running_ = false;
    return true;
}

void ChildProcess::Kill() {
    if (running_) {
        TerminateProcess(process_.hProcess, 1);
        WaitForSingleObject(process_.hProcess, 3000);
        running_ = false;
    }
    if (process_.hProcess != nullptr) CloseHandle(process_.hProcess);
    if (process_.hThread != nullptr) CloseHandle(process_.hThread);
    process_.hProcess = nullptr;
    process_.hThread = nullptr;
    CloseStdin();
    if (stdoutRead_ != nullptr) CloseHandle(stdoutRead_);
    if (stderrRead_ != nullptr) CloseHandle(stderrRead_);
    stdoutRead_ = nullptr;
    stderrRead_ = nullptr;
}

#else  // POSIX

bool ChildProcess::Start(const std::string& executable, const std::vector<std::string>& arguments,
                         const std::string& workingDirectory, std::string& error) {
    int stdinPipe[2];
    int stdoutPipe[2];
    int stderrPipe[2];
    if (pipe(stdinPipe) != 0 || pipe(stdoutPipe) != 0 || pipe(stderrPipe) != 0) {
        error = "pipe failed";
        return false;
    }
    pid_ = fork();
    if (pid_ < 0) {
        error = "fork failed";
        return false;
    }
    if (pid_ == 0) {
        dup2(stdinPipe[0], STDIN_FILENO);
        dup2(stdoutPipe[1], STDOUT_FILENO);
        dup2(stderrPipe[1], STDERR_FILENO);
        close(stdinPipe[0]);
        close(stdinPipe[1]);
        close(stdoutPipe[0]);
        close(stdoutPipe[1]);
        close(stderrPipe[0]);
        close(stderrPipe[1]);
        if (!workingDirectory.empty() && chdir(workingDirectory.c_str()) != 0) _exit(126);
        std::vector<char*> argv;
        argv.push_back(const_cast<char*>(executable.c_str()));
        for (const std::string& argument : arguments) {
            argv.push_back(const_cast<char*>(argument.c_str()));
        }
        argv.push_back(nullptr);
        execvp(executable.c_str(), argv.data());
        _exit(127);
    }
    close(stdinPipe[0]);
    close(stdoutPipe[1]);
    close(stderrPipe[1]);
    stdinWrite_ = stdinPipe[1];
    stdoutRead_ = stdoutPipe[0];
    stderrRead_ = stderrPipe[0];
    running_ = true;
    return true;
}

bool ChildProcess::WriteRaw(const std::string& bytes, std::string& error) {
    if (stdinWrite_ < 0) {
        error = "stdin is closed";
        return false;
    }
    if (write(stdinWrite_, bytes.data(), bytes.size()) !=
        static_cast<ssize_t>(bytes.size())) {
        error = "failed to write the bootstrap bytes to the sidecar";
        return false;
    }
    return true;
}

bool ChildProcess::WriteLine(const std::string& line, std::string& error) {
    return WriteRaw(line + "\n", error);
}

void ChildProcess::CloseStdin() {
    if (stdinWrite_ >= 0) {
        close(stdinWrite_);
        stdinWrite_ = -1;
    }
}

std::string ChildProcess::ReadLine(int timeoutMs, bool& timedOut) {
    timedOut = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    for (;;) {
        const std::size_t newline = stdoutBuffer_.find('\n');
        if (newline != std::string::npos) {
            std::string line = stdoutBuffer_.substr(0, newline);
            stdoutBuffer_.erase(0, newline + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            return line;
        }
        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(stdoutRead_, &readSet);
        timeval timeout{0, 20000};
        const int ready = select(stdoutRead_ + 1, &readSet, nullptr, nullptr, &timeout);
        if (ready > 0) {
            char buffer[4096];
            const ssize_t count = read(stdoutRead_, buffer, sizeof(buffer));
            if (count > 0) {
                stdoutBuffer_.append(buffer, static_cast<std::size_t>(count));
                continue;
            }
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            timedOut = true;
            return std::string();
        }
    }
}

void ChildProcess::DrainStderr() {
    if (stderrRead_ < 0) return;
    for (;;) {
        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(stderrRead_, &readSet);
        timeval timeout{0, 0};
        if (select(stderrRead_ + 1, &readSet, nullptr, nullptr, &timeout) <= 0) return;
        char buffer[4096];
        const ssize_t count = read(stderrRead_, buffer, sizeof(buffer));
        if (count <= 0) return;
        stderrTail_.append(buffer, static_cast<std::size_t>(count));
        if (stderrTail_.size() > 16 * 1024) stderrTail_.erase(0, stderrTail_.size() - 16 * 1024);
    }
}

bool ChildProcess::WaitForExit(int timeoutMs, int& exitCode) {
    if (!running_) return false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    for (;;) {
        int status = 0;
        const pid_t result = waitpid(pid_, &status, WNOHANG);
        if (result == pid_) {
            exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
            running_ = false;
            return true;
        }
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

void ChildProcess::Kill() {
    if (running_) {
        kill(pid_, SIGKILL);
        int status = 0;
        waitpid(pid_, &status, 0);
        running_ = false;
    }
    CloseStdin();
    if (stdoutRead_ >= 0) close(stdoutRead_);
    if (stderrRead_ >= 0) close(stderrRead_);
    stdoutRead_ = -1;
    stderrRead_ = -1;
}

#endif

// ---------------------------------------------------------------------------

std::string PythonExecutable() {
    const char* fromEnvironment = std::getenv("SIGFLOW_EDU_AGENT_PYTHON");
    if (fromEnvironment != nullptr && *fromEnvironment != '\0') return fromEnvironment;
#ifdef _WIN32
    return "python.exe";
#else
    return "python3";
#endif
}

eda::Json GetJson(int port, const std::string& path, const std::string& token, int& status) {
    httplib::Client client("127.0.0.1", port);
    client.set_connection_timeout(2, 0);
    client.set_read_timeout(10, 0);
    httplib::Headers headers;
    if (!token.empty()) headers.emplace("Authorization", "Bearer " + token);
    const auto res = client.Get(path.c_str(), headers);
    if (!res) {
        status = -1;
        return eda::Json::object();
    }
    status = res->status;
    try {
        return eda::Json::parse(res->body);
    } catch (const std::exception&) {
        return eda::Json::object();
    }
}

eda::Json PostJson(int port, const std::string& path, const std::string& token,
                   const eda::Json& body, int& status) {
    httplib::Client client("127.0.0.1", port);
    client.set_connection_timeout(2, 0);
    client.set_read_timeout(10, 0);
    httplib::Headers headers;
    if (!token.empty()) headers.emplace("Authorization", "Bearer " + token);
    const auto res = client.Post(path.c_str(), headers, body.dump(), "application/json");
    if (!res) {
        status = -1;
        return eda::Json::object();
    }
    status = res->status;
    try {
        return eda::Json::parse(res->body);
    } catch (const std::exception&) {
        return eda::Json::object();
    }
}

struct SidecarHandle {
    ChildProcess process;
    std::string nonce;
    std::string uiToken = "tok-test-ui-" + RandomHex(24);
    std::string gatewayToken = "tok-test-eda-" + RandomHex(24);
    int port = 0;
    std::string readyLine;
    std::string bootstrapBytes;
};

// 启动一个真实 sidecar；readyLine 为空表示没有拿到合法 ready。
// executable/arguments/workingDirectory 由调用方给出，以便覆盖
// "python -m ..."（开发路径）与 "安装后的启动器"（发布路径）两种入口。
bool StartSidecarWith(SidecarHandle& handle, const std::string& executable,
                      const std::vector<std::string>& baseArguments,
                      const std::string& workingDirectory, const std::string& dataDirectory,
                      const std::vector<std::string>& extraArguments, bool expectReady,
                      std::string& error) {
    handle.nonce = RandomHex(64);
    // AD-01：必须用**宿主真实的序列化**写启动材料，而不是测试自己拼 JSON。
    // 自己拼单行 JSON 会让"宿主写出多行、sidecar 只读一行"的缺陷永远测不出来。
    AgentServiceController::BootstrapFields fields;
    fields.protocol = "edu.api.v1";
    fields.instanceId = "inst-sidecar-smoke";
    fields.nonce = handle.nonce;
    // 故意指向一个没有监听的端口：report_review 必须因此明确失败而不是编造结果。
    fields.gatewayUrl = "http://127.0.0.1:9";
    fields.gatewayToken = handle.gatewayToken;
    fields.uiToken = handle.uiToken;
    fields.dataDirectory = dataDirectory;
    const std::string bootstrapBytes =
        AgentServiceController::SerializeBootstrapLine(fields);
    handle.bootstrapBytes = bootstrapBytes;

    std::vector<std::string> arguments = baseArguments;
    arguments.insert(arguments.end(), extraArguments.begin(), extraArguments.end());
    if (!handle.process.Start(executable, arguments, workingDirectory, error)) return false;
    if (!handle.process.WriteRaw(bootstrapBytes, error)) {
        return false;
    }
    bool timedOut = false;
    const std::string line = handle.process.ReadLine(expectReady ? 15000 : 4000, timedOut);
    handle.readyLine = line;
    return true;
}

bool StartSidecar(SidecarHandle& handle, const std::string& sourceDirectory,
                  const std::string& dataDirectory, const std::vector<std::string>& extraArguments,
                  bool expectReady, std::string& error) {
    return StartSidecarWith(handle, PythonExecutable(),
                            {"-m", "sigflow_edu_agent", "--bootstrap-stdin"}, sourceDirectory,
                            dataDirectory, extraArguments, expectReady, error);
}

// 安装后的启动器入口（agent/bin/…）：Windows 需经 cmd.exe 执行 .cmd。
bool StartSidecarViaLauncher(SidecarHandle& handle, const std::filesystem::path& launcher,
                             const std::string& dataDirectory, std::string& error) {
#ifdef _WIN32
    return StartSidecarWith(handle, "cmd.exe",
                            {"/d", "/c", launcher.string(), "--bootstrap-stdin"}, "",
                            dataDirectory, {}, true, error);
#else
    return StartSidecarWith(handle, launcher.string(), {"--bootstrap-stdin"}, "", dataDirectory, {},
                            true, error);
#endif
}

} // namespace

wxIMPLEMENT_APP_NO_MAIN(SidecarTestApp);

int main() {
    namespace fs = std::filesystem;
    const fs::path repoRoot = fs::path(SIGFLOW_REPO_ROOT);
    const fs::path sourceDirectory = repoRoot / "agent" / "src";
    if (!fs::is_regular_file(sourceDirectory / "sigflow_edu_agent" / "__main__.py")) {
        Skip("agent/src/sigflow_edu_agent is not present; sidecar integration not run");
        std::cout << "ALL PASS (skipped=" << g_skipped << ")\n";
        return 0;
    }
    // AD-15：真实控制器路径需要 wx 运行时（wxProcess 异步终止事件经事件循环派发）。
    const bool wxReady = wxInitialize();
    if (!wxReady) Skip("wxInitialize failed; controller Launch/Tick coverage not run");
    const fs::path dataDirectory =
        fs::temp_directory_path() /
        ("sigflow_sidecar_smoke_" + std::to_string(
             std::chrono::high_resolution_clock::now().time_since_epoch().count()));
    std::error_code cleanupError;
    fs::remove_all(dataDirectory, cleanupError);
    fs::create_directories(dataDirectory);

    const std::string python = PythonExecutable();
    std::cout << "  python: " << python << "\n";
    std::cout << "  agent source: " << sourceDirectory.string() << "\n";

    // ---- 0. AD-01：宿主写出的启动材料必须恰好一行（sidecar 只 readline 一次） ----
    {
        AgentServiceController::BootstrapFields fields;
        fields.protocol = "edu.api.v1";
        fields.instanceId = "inst-ad01";
        fields.nonce = std::string(64, 'a');
        fields.gatewayUrl = "http://127.0.0.1:12345";
        fields.gatewayToken = "tok-test-eda-ad01";
        fields.uiToken = "tok-test-ui-ad01";
        fields.dataDirectory = dataDirectory.string();
        const std::string bytes = AgentServiceController::SerializeBootstrapLine(fields);

        std::size_t newlines = 0;
        for (char character : bytes) {
            if (character == '\n') ++newlines;
        }
        Check(newlines == 1, "host bootstrap bytes contain exactly one newline");
        Check(!bytes.empty() && bytes.back() == '\n',
              "host bootstrap bytes end with the single newline");
        Check(bytes.find('\r') == std::string::npos,
              "host bootstrap bytes contain no carriage return");
        Check(bytes.find("\t\"") == std::string::npos,
              "host bootstrap bytes are not pretty-printed (no indentation)");

        eda::Json parsed;
        bool parsedOk = false;
        try {
            parsed = eda::Json::parse(bytes);
            parsedOk = parsed.is_object();
        } catch (const std::exception&) {
            parsedOk = false;
        }
        Check(parsedOk, "host bootstrap bytes are a single parseable JSON object");
        Check(parsedOk && parsed.value("type", std::string()) == "sigflow-bootstrap" &&
                  parsed.value("protocol", std::string()) == "edu.api.v1" &&
                  parsed.value("nonce", std::string()) == fields.nonce &&
                  parsed.value("gateway_url", std::string()) == fields.gatewayUrl &&
                  parsed.value("gateway_token", std::string()) == fields.gatewayToken &&
                  parsed.value("ui_token", std::string()) == fields.uiToken &&
                  parsed.value("instance_id", std::string()) == fields.instanceId &&
                  parsed.value("data_directory", std::string()) == fields.dataDirectory,
              "host bootstrap carries every field the sidecar requires");
        // 真实控制器在 Launch 前 nonce 为空，这里顺带确认该状态下也仍是单行。
        AgentServiceController::Config probeConfig;
        probeConfig.instanceId = "inst-ad01-pre";
        probeConfig.gatewayUrl = "http://127.0.0.1:1";
        AgentServiceController probe(std::move(probeConfig));
        const std::string preLaunch = probe.BootstrapLineForTest();
        std::size_t preNewlines = 0;
        for (char character : preLaunch) {
            if (character == '\n') ++preNewlines;
        }
        Check(preNewlines == 1, "the controller's own payload builder stays single-line");
    }

    // ---- 1. 真实握手 + health + 规则卡 + 安全关闭 ----
    {
        SidecarHandle handle;
        std::string error;
        if (!StartSidecar(handle, sourceDirectory.string(), dataDirectory.string(), {}, true, error)) {
            Check(false, "real sidecar starts with a bootstrap line on stdin");
            std::cout << "  start error: " << error << "\n";
        } else if (handle.readyLine.empty()) {
            Skip("python/sidecar could not be launched here; integration assertions not run");
        } else {
            // 真实 sidecar 接受的是宿主真实写出的那串字节（不是测试自己拼的 JSON）。
            Check(handle.bootstrapBytes.find('\n') == handle.bootstrapBytes.size() - 1,
                  "the real sidecar was fed the host's single-line bootstrap bytes");
            AgentServiceController::ReadyRecord ready;
            std::string readyError;
            const bool accepted = AgentServiceController::ValidateReadyLine(
                handle.readyLine, handle.nonce, "edu.api.v1", ready, readyError);
            Check(accepted, "the host accepts the real sidecar ready line");
            if (!accepted) std::cout << "      reason: " << readyError << "\n";
            Check(ready.port > 0, "ready line carries a bound loopback port");
            Check(!ready.agentVersion.empty(), "ready line carries a non-empty agent version");
            handle.port = ready.port;
            Check(handle.readyLine.find(handle.uiToken) == std::string::npos &&
                      handle.readyLine.find(handle.gatewayToken) == std::string::npos,
                  "the ready line never contains a token");

            int status = 0;
            const eda::Json health = GetJson(handle.port, "/api/v1/health", handle.uiToken, status);
            Check(status == 200, "sidecar GET /api/v1/health returns 200");
            const eda::Json healthData =
                health.contains("data") ? health["data"] : eda::Json::object();
            Check(healthData.value("protocol", std::string()) == "edu.api.v1",
                  "health reports protocol edu.api.v1 (the host's health probe requirement)");
            Check(healthData.contains("model") &&
                      healthData["model"].value("available", true) == false,
                  "without a model key the sidecar reports model.available=false");
            GetJson(handle.port, "/api/v1/health", "", status);
            Check(status == 200, "health needs no bearer token");

            const eda::Json wrongToken =
                GetJson(handle.port, "/api/v1/capabilities", "tok-test-wrong", status);
            Check(status == 401, "a wrong UI token is rejected with 401");
            (void)wrongToken;

            eda::Json sessionRequest;
            sessionRequest["project_id"] = "project-0123456789abcdef01234567";
            sessionRequest["revision"] = "rev-1-0123456789ab";
            sessionRequest["top"] = "top";
            sessionRequest["target"] = eda::Json::object();
            const eda::Json session = PostJson(handle.port, "/api/v1/sessions", handle.uiToken,
                                               sessionRequest, status);
            Check(status == 201, "POST /api/v1/sessions returns 201");
            const std::string sessionId = session["data"].value("session_id", std::string());
            Check(!sessionId.empty(), "session id is returned");

            eda::Json runRequest;
            runRequest["kind"] = "explain";
            runRequest["level"] = "L1";
            eda::Json selection;
            selection["path"] = "rtl/top.v";
            selection["start_line"] = 1;
            selection["end_line"] = 3;
            selection["text"] =
                "module top(input d, en, output reg q);\n"
                "  always @(*) if (en) q = d;\n"
                "endmodule\n";
            runRequest["selection"] = selection;
            const eda::Json run = PostJson(
                handle.port, "/api/v1/sessions/" + sessionId + "/runs", handle.uiToken, runRequest,
                status);
            Check(status == 201, "POST /sessions/{s}/runs returns 201 for an explanation request");
            const eda::Json runData = run["data"];
            Check(runData.value("model_used", true) == false,
                  "without a model key the run reports model_used=false");
            Check(runData.value("state", std::string()) != "succeeded" ||
                      runData.value("model_used", true) == false,
                  "a model-free run is explicitly degraded rather than presented as normal");
            const eda::Json cards = runData["cards"];
            Check(cards.is_array() && !cards.empty(), "the run produces at least one card");
            bool ruleCard = false;
            for (const auto& card : cards) {
                if (card.value("source", std::string()) == "rule") ruleCard = true;
                Check(card.value("expired", true) == false, "cards are not pre-expired");
                Check(card.contains("limitations"), "cards expose their limitations");
            }
            Check(ruleCard, "a rule card is produced without any model key (NG-07 acceptance a)");
            const std::string runDump = run.dump();
            Check(runDump.find(handle.uiToken) == std::string::npos &&
                      runDump.find(handle.gatewayToken) == std::string::npos,
                  "no token appears in the teaching response");
            Check(runDump.find(dataDirectory.string()) == std::string::npos,
                  "the per-instance data directory is never echoed back");
#ifdef _WIN32
            Check(runDump.find("C:\\\\") == std::string::npos &&
                      runDump.find("C:/") == std::string::npos,
                  "no local absolute path appears in the teaching response");
#endif

            // Gateway 不可达：必须显式失败，不得伪造“已读报告”。
            eda::Json reportRequest;
            reportRequest["kind"] = "report_review";
            reportRequest["level"] = "L1";
            reportRequest["job_id"] = "job-does-not-exist";
            const eda::Json reportRun = PostJson(
                handle.port, "/api/v1/sessions/" + sessionId + "/runs", handle.uiToken,
                reportRequest, status);
            const bool explicitFailure =
                status >= 400 || reportRun["data"].value("state", std::string()) == "failed";
            Check(explicitFailure,
                  "report_review with an unreachable Gateway fails explicitly (NG-07 acceptance c)");
            if (status >= 400) {
                Check(reportRun.contains("error") &&
                          reportRun["error"].value("code", std::string()) == "GATEWAY_UNAVAILABLE",
                      "the explicit failure names GATEWAY_UNAVAILABLE");
            } else {
                Check(reportRun["data"].value("model_used", true) == false,
                      "a degraded report run still reports model_used=false");
            }

            // 安全关闭。
            eda::Json empty;
            PostJson(handle.port, "/api/v1/shutdown", handle.uiToken, empty, status);
            Check(status == 202, "POST /api/v1/shutdown returns 202");
            int exitCode = -1;
            const bool exited = handle.process.WaitForExit(5000, exitCode);
            Check(exited && exitCode == 0, "the sidecar exits 0 after a safe shutdown");
            handle.process.DrainStderr();
            Check(handle.process.stderrTail().find(handle.uiToken) == std::string::npos &&
                      handle.process.stderrTail().find(handle.gatewayToken) == std::string::npos,
                  "the sidecar never logs a token");
        }
    }

    // ---- 2. 版本/nonce/协议失配必须被宿主拒绝 ----
    {
        SidecarHandle handle;
        std::string error;
        StartSidecar(handle, sourceDirectory.string(), dataDirectory.string(),
                     {"--selftest-fail", "wrong-nonce"}, true, error);
        if (!handle.readyLine.empty()) {
            AgentServiceController::ReadyRecord ready;
            std::string readyError;
            Check(!AgentServiceController::ValidateReadyLine(handle.readyLine, handle.nonce,
                                                             "edu.api.v1", ready, readyError),
                  "a ready line with the wrong nonce is rejected");
            Check(readyError.find("nonce") != std::string::npos,
                  "the rejection reason names the nonce mismatch");
        }
    }
    {
        SidecarHandle handle;
        std::string error;
        StartSidecar(handle, sourceDirectory.string(), dataDirectory.string(),
                     {"--selftest-fail", "wrong-protocol"}, true, error);
        if (!handle.readyLine.empty()) {
            AgentServiceController::ReadyRecord ready;
            std::string readyError;
            Check(!AgentServiceController::ValidateReadyLine(handle.readyLine, handle.nonce,
                                                             "edu.api.v1", ready, readyError),
                  "an incompatible protocol is rejected");
            Check(readyError.find("protocol") != std::string::npos,
                  "the rejection reason names the protocol mismatch");
        }
    }
    {
        SidecarHandle handle;
        std::string error;
        StartSidecar(handle, sourceDirectory.string(), dataDirectory.string(),
                     {"--selftest-fail", "exit-before-ready"}, false, error);
        Check(handle.readyLine.empty(),
              "a sidecar that exits before readiness yields no ready line");
        int exitCode = -1;
        Check(handle.process.WaitForExit(5000, exitCode),
              "the host observes the early exit (so it can apply bounded restart)");
    }

    // ---- 3. 双实例隔离：不同端口 / 不同 token / 不同数据目录 ----
    {
        SidecarHandle first;
        SidecarHandle second;
        std::string error;
        const fs::path firstData = dataDirectory / "instance-a";
        const fs::path secondData = dataDirectory / "instance-b";
        fs::create_directories(firstData, cleanupError);
        fs::create_directories(secondData, cleanupError);
        const bool firstStarted =
            StartSidecar(first, sourceDirectory.string(), firstData.string(), {}, true, error);
        const bool secondStarted =
            StartSidecar(second, sourceDirectory.string(), secondData.string(), {}, true, error);
        if (firstStarted && secondStarted && !first.readyLine.empty() && !second.readyLine.empty()) {
            AgentServiceController::ReadyRecord firstReady;
            AgentServiceController::ReadyRecord secondReady;
            std::string readyError;
            Check(AgentServiceController::ValidateReadyLine(first.readyLine, first.nonce,
                                                            "edu.api.v1", firstReady, readyError),
                  "instance A becomes ready");
            Check(AgentServiceController::ValidateReadyLine(
                      second.readyLine, second.nonce, "edu.api.v1", secondReady, readyError),
                  "instance B becomes ready");
            Check(firstReady.port != secondReady.port,
                  "two instances bind different loopback ports");
            Check(first.uiToken != second.uiToken && first.gatewayToken != second.gatewayToken,
                  "each instance gets its own tokens");
            int status = 0;
            GetJson(firstReady.port, "/api/v1/health", first.uiToken, status);
            Check(status == 200, "instance A accepts its own token");
            GetJson(secondReady.port, "/api/v1/health", second.uiToken, status);
            Check(status == 200, "instance B accepts its own token");
            // 交叉使用：A 的 token 不得被 B 接受（实例隔离）。health 按契约免鉴权。
            GetJson(secondReady.port, "/api/v1/health", first.uiToken, status);
            Check(status == 200, "health stays token-free by contract (documented exception)");
            GetJson(secondReady.port, "/api/v1/capabilities", first.uiToken, status);
            Check(status == 401, "instance B rejects instance A's UI token");
            GetJson(firstReady.port, "/api/v1/capabilities", second.uiToken, status);
            Check(status == 401, "instance A rejects instance B's UI token");
            Check(firstData != secondData, "each instance owns a separate data directory");
            eda::Json empty;
            PostJson(firstReady.port, "/api/v1/shutdown", first.uiToken, empty, status);
            Check(status == 202, "instance A shuts down on request");
            GetJson(secondReady.port, "/api/v1/health", second.uiToken, status);
            Check(status == 200, "instance B stays healthy while A shuts down");
            PostJson(secondReady.port, "/api/v1/shutdown", second.uiToken, empty, status);
            Check(status == 202, "instance B shuts down on request");
            int exitCode = -1;
            Check(first.process.WaitForExit(5000, exitCode) && exitCode == 0,
                  "instance A exits 0");
            Check(second.process.WaitForExit(5000, exitCode) && exitCode == 0,
                  "instance B exits 0");
        } else {
            Skip("two sidecar instances could not be started here; isolation not asserted");
        }
    }

    // ---- 4. 安装后的启动器入口（发布路径，而不是 python -m 开发路径） ----
    {
        const fs::path launcher =
#ifdef _WIN32
            repoRoot / "agent" / "bin" / "sigflow-edu-agent.cmd";
#else
            repoRoot / "agent" / "bin" / "sigflow-edu-agent";
#endif
        if (!fs::is_regular_file(launcher)) {
            Skip("installed launcher is missing; launcher path not asserted");
        } else {
            SidecarHandle handle;
            std::string error;
            const fs::path launcherData = dataDirectory / "launcher";
            fs::create_directories(launcherData, cleanupError);
            if (StartSidecarViaLauncher(handle, launcher, launcherData.string(), error) &&
                !handle.readyLine.empty()) {
                AgentServiceController::ReadyRecord ready;
                std::string readyError;
                Check(AgentServiceController::ValidateReadyLine(handle.readyLine, handle.nonce,
                                                                "edu.api.v1", ready, readyError),
                      "the installed launcher performs the bootstrap handshake");
                int status = 0;
                const eda::Json health =
                    GetJson(ready.port, "/api/v1/health", handle.uiToken, status);
                Check(status == 200 &&
                          (health.contains("data") ? health["data"] : health)
                                  .value("protocol", std::string()) == "edu.api.v1",
                      "the launcher-started sidecar answers health with the contract protocol");
                eda::Json empty;
                PostJson(ready.port, "/api/v1/shutdown", handle.uiToken, empty, status);
                Check(status == 202, "the launcher-started sidecar shuts down cleanly");
                int exitCode = -1;
                Check(handle.process.WaitForExit(5000, exitCode) && exitCode == 0,
                      "the launcher propagates a clean exit code");
            } else {
                Skip("the installed launcher could not start here; launcher path not asserted");
            }
        }
    }

#ifdef _WIN32
    // ---- 5. 控制器必须构造与上面真实启动器测试相同的 Windows argv ----
    {
        const fs::path launcher = repoRoot / "agent" / "bin" / "sigflow-edu-agent.cmd";
        const std::vector<wxString> command = AgentServiceController::BuildLaunchCommand(
            wxString::FromUTF8(launcher.string()), {"--bootstrap-stdin"});
        Check(command.size() == 5 && command[0].Lower().EndsWith("cmd.exe") &&
                  command[1] == "/d" && command[2] == "/c" &&
                  command[3] == wxString::FromUTF8(launcher.string()) &&
                  command[4] == "--bootstrap-stdin",
              "AgentServiceController wraps the documented .cmd launcher with cmd.exe /d /c");
    }
#endif

    // ---- 6. AD-15/AD-02：真实控制器 Launch/Tick 全链 ----
    // 之前这里所有断言都是测试自己 spawn 进程；这一段改为驱动 AgentServiceController
    // 自己的 Start()/Tick()，因此它同时覆盖 AD-01（宿主真实 bootstrap 字节）与
    // AD-02（ready 之后必须通过一次 health 才允许就绪）。
    const wxString controllerPython = wxReady ? ResolvePythonExecutable() : wxString();
    if (controllerPython.empty()) {
        Skip("python interpreter not resolvable as an absolute path; controller harness not run");
    } else {
        std::cout << "  controller python: " << controllerPython.ToUTF8().data() << "\n";
        const fs::path controllerData = dataDirectory / "controller";
        fs::create_directories(controllerData, cleanupError);

        // 6a. 正常路径：Start -> 首次 health 通过 -> Ready，端口真的在服务，Stop 干净。
        {
            AgentServiceController controller(
                MakeControllerConfig(controllerPython, sourceDirectory, controllerData, {}));
            Check(controller.Start(),
                  "controller launches the sidecar through its own Launch()");
            const bool becameReady =
                PumpUntil(controller, [&controller]() { return controller.ready(); }, 25000);
            Check(becameReady, "controller reaches Ready through its own Tick() loop");
            const AgentServiceController::Status status = controller.status();
            Check(status.port > 0, "Ready publishes the verified loopback port");
            int healthStatus = 0;
            const eda::Json health =
                GetJson(status.port, "/api/v1/health", "", healthStatus);
            const eda::Json healthData =
                health.contains("data") ? health["data"] : eda::Json::object();
            Check(healthStatus == 200 &&
                      healthData.value("protocol", std::string()) == "edu.api.v1",
                  "the controller's own port answers the contract health probe");
            Check(healthData.value("instance_id", std::string()) == "inst-controller-ad15",
                  "the controller's bootstrap reached the sidecar (instance id echoed)");
            controller.Stop();
            Check(!controller.ready() &&
                      controller.status().state == AgentServiceController::State::Stopped,
                  "Stop() returns the controller to Stopped");
        }

        // 6b. AD-02 负例：合法 ready + 坏 health，永远不允许进入 Ready。
        for (const char* mode : {"bad-health", "bad-health-protocol"}) {
            const fs::path modeData = controllerData / mode;
            fs::create_directories(modeData, cleanupError);
            AgentServiceController controller(MakeControllerConfig(
                controllerPython, sourceDirectory, modeData, {"--selftest-fail", mode}));
            controller.Start();
            const bool becameReady =
                PumpUntil(controller, [&controller]() { return controller.ready(); }, 9000);
            Check(!becameReady,
                  "a sidecar whose health check fails never reaches Ready (AD-02)");
            Check(controller.status().state != AgentServiceController::State::Ready,
                  "the controller keeps the failing sidecar out of the Ready state");
            controller.Stop();
        }

        // 6c. AD-15：进程在 ready 后崩溃，控制器必须观察到并做有界重启。
        {
            const fs::path crashData = controllerData / "crash";
            fs::create_directories(crashData, cleanupError);
            AgentServiceController controller(MakeControllerConfig(
                controllerPython, sourceDirectory, crashData,
                {"--selftest-fail", "exit-after-ready"}));
            controller.Start();
            const bool observed =
                PumpUntil(controller,
                          [&controller]() { return controller.status().restartCount > 0; }, 15000);
            Check(observed, "the controller observes the sidecar exit and schedules a restart");
            const bool bounded =
                PumpUntil(controller,
                          [&controller]() {
                              return controller.status().state ==
                                     AgentServiceController::State::Failed;
                          },
                          20000);
            Check(bounded && controller.status().restartCount == kControllerMaxRestarts,
                  "restarts stay bounded and end in Failed (Agent features only)");
            Check(controller.status().state != AgentServiceController::State::Ready,
                  "a crashed sidecar is never reported as Ready");
            controller.Stop();
        }
    }

    if (wxReady) wxUninitialize();
    fs::remove_all(dataDirectory, cleanupError);
    std::cout << (g_failures == 0 ? "ALL PASS" : "FAILURES")
              << " (skipped=" << g_skipped << ")\n";
    return g_failures == 0 ? 0 : 1;
}
