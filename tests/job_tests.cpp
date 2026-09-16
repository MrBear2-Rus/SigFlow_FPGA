// job_tests.cpp — jobs 层冒烟测试（无 GUI；链接 wxBase + jsoncpp）
#include "jobs/PlatformProcess.h"
#include "jobs/JobService.h"
#include "jobs/ToolJobs.h"
#include "jobs/JobRunner.h"

#include "platform/PlatformPaths.h"
#include "platform/DynamicLibrary.h"
#include "platform/LocalPipe.h"

#include <wx/file.h>
#include <wx/filename.h>
#include <wx/dir.h>

#include <filesystem>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>

static int g_failures = 0;
#define CHECK(cond, msg)                                                      \
    do {                                                                      \
        if (cond) { std::printf("[PASS] %s\n", msg); }                        \
        else { std::printf("[FAIL] %s\n", msg); ++g_failures; }               \
    } while (0)

static wxString MakeTempProjectDir()
{
    const std::filesystem::path base =
        std::filesystem::temp_directory_path() / "sigflow_job_tests";
    std::filesystem::create_directories(base);
    const std::filesystem::path dir = base /
        ("run-" + std::to_string(static_cast<long long>(std::time(nullptr))));
    std::filesystem::create_directories(dir);
    return wxString(dir.string());
}

static void TestPlatformProcess()
{
    // 注意：这里的用例必须按平台分支。
    // 原实现无条件下硬编码 cmd.exe / ping -n，导致 Linux 上 ctest 直接 6 项失败，
    // 而"失败"又被当成环境问题忽略 —— 等于 jobs 层在 Linux 上从未被真正验证过。
#if defined(_WIN32)
    PlatformProcessRequest echo;
    echo.executable = "cmd.exe";
    echo.arguments = { "/d", "/s", "/c", "echo hello-from-job" };
    echo.timeoutSeconds = 30;
    const PlatformProcessResult echoResult = PlatformProcess::Run(echo);
    CHECK(echoResult.started && echoResult.exitCode == 0, "cmd echo exits 0");
    CHECK(echoResult.output.Contains("hello-from-job"), "stdout captured");

    // 真实往返：带空格的完整参数必须原样到达子进程
    PlatformProcessRequest round;
    round.executable = "cmd.exe";
    round.arguments = { "/d", "/s", "/c", "echo", "a b c" };
    round.timeoutSeconds = 30;
    const PlatformProcessResult roundResult = PlatformProcess::Run(round);
    CHECK(roundResult.started && roundResult.exitCode == 0 &&
          roundResult.output.Contains("a b c"),
          "argv round-trip preserves spaced argument");

    // 原始命令行模式：executable 作为完整命令行原样传递，不做二次转义
    PlatformProcessRequest rawMode;
    rawMode.executable = "cmd.exe /d /s /c echo raw-mode-ok";
    rawMode.quoteArguments = false;
    rawMode.timeoutSeconds = 30;
    const PlatformProcessResult rawResult = PlatformProcess::Run(rawMode);
    CHECK(rawResult.started && rawResult.exitCode == 0 &&
          rawResult.output.Contains("raw-mode-ok"),
          "raw command line passes through unescaped");

    PlatformProcessRequest slow;
    slow.executable = "cmd.exe";
    slow.arguments = { "/d", "/s", "/c", "ping -n 5 127.0.0.1 >nul" };
    slow.timeoutSeconds = 1;
    const PlatformProcessResult slowResult = PlatformProcess::Run(slow);
    CHECK(slowResult.started && slowResult.timedOut, "timeout kills process tree");
#else
    // 用**裸命令名** "sh" 而不是 "/bin/sh"：这正好是回归用例——
    // POSIX 分支曾经用 execv()（不搜索 PATH），裸命令名必然 ENOENT/_exit(127)。
    // 现在实现改用 execvp()，所以本用例必须通过；一旦有人改回 execv，这里立刻变红。
    PlatformProcessRequest echo;
    echo.executable = "sh";
    echo.arguments = { "-c", "echo hello-from-job" };
    echo.timeoutSeconds = 30;
    const PlatformProcessResult echoResult = PlatformProcess::Run(echo);
    CHECK(echoResult.started && echoResult.exitCode == 0, "sh echo exits 0");
    CHECK(echoResult.output.Contains("hello-from-job"), "stdout captured");

    PlatformProcessRequest round;
    round.executable = "sh";
    round.arguments = { "-c", "echo \"$1\"", "sh", "a b c" };
    round.timeoutSeconds = 30;
    const PlatformProcessResult roundResult = PlatformProcess::Run(round);
    CHECK(roundResult.started && roundResult.exitCode == 0 &&
          roundResult.output.Contains("a b c"),
          "argv round-trip preserves spaced argument");

    // 原始命令行模式在 POSIX 上明确不支持（没有可移植的 shell 解析）：
    // 断言"被拒绝并给出错误信息"，而不是静默当成成功。
    PlatformProcessRequest rawMode;
    rawMode.executable = "echo raw-mode-ok";
    rawMode.quoteArguments = false;
    rawMode.timeoutSeconds = 30;
    const PlatformProcessResult rawResult = PlatformProcess::Run(rawMode);
    CHECK(!rawResult.started && !rawResult.errorMessage.IsEmpty(),
          "raw command line rejected on POSIX with an error");

    PlatformProcessRequest slow;
    slow.executable = "sh";
    slow.arguments = { "-c", "sleep 5" };
    slow.timeoutSeconds = 1;
    const PlatformProcessResult slowResult = PlatformProcess::Run(slow);
    CHECK(slowResult.started && slowResult.timedOut, "timeout kills process tree");
#endif

    const wxString quoted = PlatformProcess::BuildCommandLine("tool.exe",
        { "C:\\ends with backslash\\", "a b" });
    CHECK(quoted == "tool.exe \"C:\\ends with backslash\\\\\" \"a b\"",
          "canonical backslash quoting");
}

static void TestProcessEnvironment()
{
#if !defined(_WIN32)
    PlatformProcessRequest request;
    request.executable = "sh";
    request.arguments = { "-c", "echo $SIGFLOW_TEST_ENV" };
    request.environment = { { "SIGFLOW_TEST_ENV", "env-override-ok" } };
    request.timeoutSeconds = 30;
    const PlatformProcessResult result = PlatformProcess::Run(request);
    CHECK(result.started && result.exitCode == 0 && result.output.Contains("env-override-ok"),
          "environment override reaches child process");
#else
    PlatformProcessRequest request;
    request.executable = "cmd.exe";
    request.arguments = { "/d", "/s", "/c", "echo %SIGFLOW_TEST_ENV%" };
    request.environment = { { "SIGFLOW_TEST_ENV", "env-override-ok" } };
    request.timeoutSeconds = 30;
    const PlatformProcessResult result = PlatformProcess::Run(request);
    CHECK(result.started && result.exitCode == 0 && result.output.Contains("env-override-ok"),
          "environment override reaches child process");
#endif
}

static void TestJobServiceStateMachine()
{
    const wxString project = MakeTempProjectDir();
    ToolJob job;
    wxString error;

    ToolJobRequest request;
    request.type = ToolJobType::Simulation;
    request.projectPath = project;
    CHECK(JobService().Create(request, job, error), "job created");
    CHECK(wxFileExists(JobService::GetPaths(project, ToolJobType::Simulation,
                                            job.id).manifest), "manifest written");

    ToolJob loaded;
    CHECK(JobService().Load(project, job.id, loaded, error) &&
          loaded.state == ToolJobState::Created, "job loads back");

    // 非法迁移必须被拒绝：Created -> Running
    CHECK(!JobService().Transition(project, job.id, ToolJobState::Running,
                                   "illegal", 0, error), "illegal transition rejected");

    CHECK(JobService().Start(project, job.id, error) &&
          JobService().Load(project, job.id, loaded, error) &&
          loaded.state == ToolJobState::Running, "Start walks to Running");

    CHECK(JobService().Cancel(project, job.id, "test", error) &&
          JobService().Load(project, job.id, loaded, error) &&
          loaded.state == ToolJobState::Cancelled, "Cancel reaches Cancelled");

    ToolJob retryJob;
    CHECK(JobService().Retry(project, job.id, retryJob, error) &&
          retryJob.retryOf == job.id, "retry links retryOf");

    std::vector<ToolJob> jobs;
    CHECK(JobService().List(project, jobs, error) && jobs.size() == 2, "list finds both");

    // 清理：直接删除整个临时项目
    std::error_code ec;
    std::filesystem::remove_all(std::filesystem::path(project.ToStdString()), ec);
}

static void TestJobRecovery()
{
    const wxString project = MakeTempProjectDir();
    ToolJob running;
    wxString error;
    ToolJobRequest request;
    request.type = ToolJobType::Simulation;
    request.projectPath = project;
    if (!JobService().Create(request, running, error) ||
        !JobService().Start(project, running.id, error)) {
        std::printf("[FAIL] setup running job for recovery test\n");
        ++g_failures;
        return;
    }

    // 保留活动作业：不应被回收。
    CHECK(JobService().RecoverStaleJobs(project, { running.id }, error), "recover with active id succeeds");
    ToolJob stillRunning;
    CHECK(JobService().Load(project, running.id, stillRunning, error) &&
          stillRunning.state == ToolJobState::Running, "active job is preserved");

    // 非活动 Running 视为陈旧 → Failed。
    CHECK(JobService().RecoverStaleJobs(project, {}, error), "recover stale succeeds");
    ToolJob recovered;
    CHECK(JobService().Load(project, running.id, recovered, error) &&
          recovered.state == ToolJobState::Failed, "stale running job recovered to Failed");

    std::error_code ec;
    std::filesystem::remove_all(std::filesystem::path(project.ToStdString()), ec);
}

static void TestJobRecoveryCrossType()
{
    const wxString project = MakeTempProjectDir();
    wxString error;
    ToolJobRequest simRequest;
    simRequest.type = ToolJobType::Simulation;
    simRequest.projectPath = project;
    ToolJob simJob;
    ToolJobRequest packRequest;
    packRequest.type = ToolJobType::Pack;
    packRequest.projectPath = project;
    ToolJob packJob;
    if (!JobService().Create(simRequest, simJob, error) ||
        !JobService().Start(project, simJob.id, error) ||
        !JobService().Create(packRequest, packJob, error) ||
        !JobService().Start(project, packJob.id, error)) {
        std::printf("[FAIL] setup cross-type running jobs for recovery test\n");
        ++g_failures;
        return;
    }

    // 只保留活动的 Pack 作业：Simulation 作业应被判定为陈旧。
    CHECK(JobService().RecoverStaleJobs(project, { packJob.id }, error),
          "cross-type recover succeeds");
    ToolJob reloadedSim;
    ToolJob reloadedPack;
    CHECK(JobService().Load(project, simJob.id, reloadedSim, error) &&
          reloadedSim.state == ToolJobState::Failed,
          "non-active simulation job recovered to Failed");
    CHECK(JobService().Load(project, packJob.id, reloadedPack, error) &&
          reloadedPack.state == ToolJobState::Running,
          "active pack job preserved across types");

    std::error_code ec;
    std::filesystem::remove_all(std::filesystem::path(project.ToStdString()), ec);
}

static void TestSimulationJobInProcess()
{
    const wxString project = MakeTempProjectDir();
    SimulationJobRequest simRequest;
    simRequest.projectPath = project;
    simRequest.topModule = "top";
    simRequest.sourceFiles = {sigflow::platform::JoinPath(project, "src.v") };
    simRequest.requireVcd = false; // 编译型：无 VCD 产物
    simRequest.runner = [](JobReport& report, wxString& runnerError) {
        report.summary = "in-process ok";
        return true;
    };

    ToolJob simJob;
    wxString error;
    CHECK(SimulationJob().Submit(simRequest, simJob, error), "sim job submitted");

    JobReport report;
    const bool ok = SimulationJob().Execute(simRequest, simJob, {}, report, error);
    CHECK(ok && report.state == ToolJobState::Succeeded, "sim job executes to Succeeded");

    ToolJob loaded;
    JobService().Load(project, simJob.id, loaded, error);
    CHECK(loaded.state == ToolJobState::Succeeded, "manifest reflects Succeeded");

    std::error_code ec;
    std::filesystem::remove_all(std::filesystem::path(project.ToStdString()), ec);
}

static void TestPlatformPaths()
{
    const wxString base = "yosys";
    CHECK(sigflow::platform::WithExecutableSuffix(base) == base + sigflow::platform::ExecutableSuffix(),
          "executable suffix appended");
    CHECK(sigflow::platform::WithExecutableSuffix(base + sigflow::platform::ExecutableSuffix()) ==
              base + sigflow::platform::ExecutableSuffix(),
          "existing suffix not duplicated");
    const wxString joined = wxString("a") + sigflow::platform::PathListSeparator() + "b" +
                            sigflow::platform::PathListSeparator() + "c";
    const std::vector<wxString> parts = sigflow::platform::SplitPathVariable(joined);
    CHECK(parts.size() == 3 && parts[0] == "a" && parts[1] == "b" && parts[2] == "c",
          "PATH list split by platform separator");
}

static void TestDynamicLibrary()
{
#if defined(_WIN32)
    sigflow::platform::DynamicLibrary library;
    CHECK(library.Load("kernel32.dll") && library.IsLoaded(), "load system DLL");
    CHECK(library.Symbol("GetCurrentProcessId") != nullptr, "resolve exported symbol");
    library.Unload();
    CHECK(!library.IsLoaded(), "unload system DLL");
#else
    sigflow::platform::DynamicLibrary library;
    CHECK(library.Load("libc.so.6") || library.Load("libdl.so.2"), "load system SO");
    if (library.IsLoaded()) library.Unload();
#endif
}

static void TestLocalPipe()
{
    std::unique_ptr<sigflow::platform::LocalPipe> host;
    std::unique_ptr<sigflow::platform::LocalPipe> device;
    std::string error;
    if (!sigflow::platform::LocalPipe::CreatePair("sigflow_job_tests_pipe", host, device, error)) {
        std::printf("[FAIL] LocalPipe::CreatePair: %s\n", error.c_str());
        ++g_failures;
        return;
    }
    CHECK(host != nullptr && device != nullptr, "pipe pair created");
    // CreatePair 已完成构造并打开两端（Windows 上 host=client, device=server）。
    const std::uint8_t out[4] = { 1, 2, 3, 4 };
    std::uint8_t in[4] = { 0, 0, 0, 0 };
    CHECK(host->Write(out, sizeof(out), 1000), "pipe write");
    const std::size_t read = device->Read(in, sizeof(in), 1000);
    CHECK(read == sizeof(in) && in[0] == 1 && in[3] == 4, "pipe read round-trip");
}

// 进程输出的解码回归测试。
//
// 旧实现直接 wxString::FromUTF8(buffer, n)：
//   * 4096 字节的 read 很容易把 3 字节汉字劈成两半 → FromUTF8 返回空串 → **整块 4KB 被丢弃**；
//   * Windows 上工具输出 CP936/GBK 时整块非法 → 同样整块丢弃。
// 中文报错信息恰好是用户最需要看到的部分，所以这两条必须作为回归用例。
static void TestOutputDecoding()
{
#if !defined(_WIN32)
    // 1) 2000 个汉字（3 字节/字 = 6000 字节）必然跨越 4096 的读取边界。
    PlatformProcessRequest big;
    big.executable = "sh";
    big.arguments = { "-c",
        "i=0; while [ $i -lt 2000 ]; do printf '\\344\\270\\255'; i=$((i+1)); done" };
    big.timeoutSeconds = 30;
    const PlatformProcessResult bigResult = PlatformProcess::Run(big);
    CHECK(bigResult.started && bigResult.exitCode == 0, "utf8 producer exits 0");
    CHECK(bigResult.output.length() == 2000,
          "utf8 output spanning read boundaries is not truncated or dropped");

    // 2) 非法 UTF-8（0xFF 0xFE）必须回退解码，而不是丢掉整块。
    PlatformProcessRequest bad;
    bad.executable = "sh";
    bad.arguments = { "-c", "printf '\\377\\376BAD'" };
    bad.timeoutSeconds = 30;
    const PlatformProcessResult badResult = PlatformProcess::Run(bad);
    CHECK(badResult.started && badResult.exitCode == 0, "binary producer exits 0");
    CHECK(badResult.output.Contains("BAD"),
          "invalid utf-8 output falls back instead of being dropped");
#endif
}

// JobRunHandle 生命周期回归测试。
//
// 旧实现把 handle 自己捕获进了线程闭包：一旦调用方丢弃最后一个引用，
// handle 就会在 **worker 线程** 上析构 → ~JobRunHandle() → m_thread.join() 自连接
// → 在 noexcept 析构里抛 system_error → std::terminate（整个进程被终止）。
// 因此本用例真正的断言是"进程还能走到下一行"。
static void TestJobHandleLifecycle()
{
    const wxString project = MakeTempProjectDir();
    ToolJob job;
    wxString error;
    ToolJobRequest request;
    request.type = ToolJobType::Simulation;
    request.projectPath = project;
    if (!JobService().Create(request, job, error)) {
        CHECK(false, "job created for handle test");
        return;
    }

    {
        // 故意不 Join，立刻释放最后一个引用。
        auto handle = RunJobAsync(
            job,
            [](const ToolJob&, const JobExecutionOptions&, JobReport& report, wxString& message) {
                report.state = ToolJobState::Succeeded;
                report.summary = "ok";
                message = "ok";
                return true;
            },
            JobExecutionOptions(),
            [](const JobRunOutcome&) {});
        (void)handle;
    }

    // 等 worker 收尾（测试环境没有 wxApp 事件循环，CallAfter 不会被派发）。
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    CHECK(true, "dropping the last JobRunHandle does not self-join / terminate");

    std::error_code ec;
    std::filesystem::remove_all(std::filesystem::path(project.ToStdString()), ec);
}

int main()
{
    TestPlatformProcess();
    TestProcessEnvironment();
    TestOutputDecoding();
    TestJobHandleLifecycle();
    TestJobServiceStateMachine();
    TestJobRecovery();
    TestJobRecoveryCrossType();
    TestSimulationJobInProcess();
    TestPlatformPaths();
    TestDynamicLibrary();
    TestLocalPipe();
    std::printf(g_failures == 0 ? "\nALL TESTS PASSED\n" : "\n%d TEST(S) FAILED\n",
                g_failures);
    return g_failures == 0 ? 0 : 1;
}
