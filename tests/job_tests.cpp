// job_tests.cpp — jobs 层冒烟测试（无 GUI；链接 wxBase + jsoncpp）
#include "jobs/PlatformProcess.h"
#include "jobs/JobService.h"
#include "jobs/ToolJobs.h"

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
#include <memory>
#include <string>

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

    const wxString quoted = PlatformProcess::BuildCommandLine("tool.exe",
        { "C:\\ends with backslash\\", "a b" });
    CHECK(quoted == "tool.exe \"C:\\ends with backslash\\\\\" \"a b\"",
          "canonical backslash quoting");
}

static void TestProcessEnvironment()
{
    PlatformProcessRequest request;
    request.executable = "cmd.exe";
    request.arguments = { "/d", "/s", "/c", "echo %SIGFLOW_TEST_ENV%" };
    request.environment = { { "SIGFLOW_TEST_ENV", "env-override-ok" } };
    request.timeoutSeconds = 30;
    const PlatformProcessResult result = PlatformProcess::Run(request);
    CHECK(result.started && result.exitCode == 0 && result.output.Contains("env-override-ok"),
          "environment override reaches child process");
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
    simRequest.sourceFiles = { project + "\\src.v" };
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

int main()
{
    TestPlatformProcess();
    TestProcessEnvironment();
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
