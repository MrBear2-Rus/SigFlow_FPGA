#include "SimulationEngine.h"
#include "StimulusParser.h"
#include "TimelineGenerator.h"
#include "SimMainGenerator.h"
#include "../jobs/PlatformProcess.h"
#include "../jobs/JobService.h"
#include <wx/stdpaths.h>
#include <wx/filename.h>
#include <wx/dir.h>
#include <filesystem>
#include <cstdio>
#include "../platform/Log.h"
#include "../platform/PlatformPaths.h"
#include "../platform/SimToolchain.h"

using sigflow::platform::JoinPath;

namespace fs = std::filesystem;

static bool SimCancelRequested(const wxString& projectPath, const wxString& jobId)
{
    return !jobId.IsEmpty() && JobService::IsCancelRequested(projectPath, jobId);
}

SimulationEngine::SimulationEngine()
    : m_projectRoot(wxGetCwd())
    , m_progressCallback(nullptr)
{
}

SimulationEngine::~SimulationEngine()
{
}

void SimulationEngine::SetProjectRoot(const wxString& projectRoot)
{
    m_projectRoot = projectRoot;
}

void SimulationEngine::SetProgressCallback(CompileProgressCallback callback)
{
    m_progressCallback = callback;
}

wxString SimulationEngine::GetCacheDirectory(const wxString& topModule) const
{
    wxFileName cacheDir(m_projectRoot, "");
    cacheDir.AppendDir(".sigflow");
    cacheDir.AppendDir("sim");
    cacheDir.AppendDir(topModule);
    return cacheDir.GetPath();
}

wxString SimulationEngine::GetObjDirPath(const wxString& topModule) const
{
    return JoinPath(GetCacheDirectory(topModule), "obj_dir");
}

bool SimulationEngine::CreateDirectoryRecursive(const wxString& path)
{
    try {
        fs::path p(fs::u8path(path.ToUTF8().data()));
        fs::create_directories(p);
        return true;
    }
    catch (const std::exception& e) {
        wxLogError("创建目录失败: %s - %s", path, e.what());
        return false;
    }
}

void SimulationEngine::SetJobContext(const wxString& projectPath, const wxString& jobId)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_jobProjectPath = projectPath;
    m_jobId = jobId;
}

PlatformProcessResult SimulationEngine::RunTool(const wxString& executable,
                                                const std::vector<wxString>& arguments,
                                                const wxString& workingDirectory,
                                                 int timeoutSeconds,
                                                 bool streamOutput,
                                                 bool rawCommandLine,
                                                 const std::vector<std::pair<wxString, wxString>>& environment)
{
    PlatformProcessRequest request;
    request.executable = executable;
    request.arguments = arguments;
    request.workingDirectory = workingDirectory;
    request.timeoutSeconds = timeoutSeconds > 0 ? timeoutSeconds : 600;
    request.maxOutputBytes = 16ull * 1024ull * 1024ull; // 16MB 日志上限，超限截断不静默膨胀
    request.quoteArguments = !rawCommandLine;
    request.environment = environment;
    request.onStarted = [this](void* handle) {
        if (!m_jobId.IsEmpty()) {
            JobService::RegisterProcess(m_jobProjectPath, m_jobId, handle);
        }
    };
    request.onFinished = [this]() {
        if (!m_jobId.IsEmpty()) {
            JobService::UnregisterProcess(m_jobProjectPath, m_jobId);
        }
    };
    PlatformOutputCallback sink;
    if (streamOutput) {
        sink = [this](const wxString& chunk, bool isError) {
            CompileOutputCallback callback;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_lastCompileLog += chunk;
                callback = m_outputCallback;
            }
            if (callback) callback(chunk, isError);
        };
    }
    const PlatformProcessResult result = PlatformProcess::Run(request, sink);
    return result;
}

wxString SimulationEngine::FindVerilatorPath() const
{
    wxString configuredPath;
    if (wxGetEnv("VERILATOR_BIN", &configuredPath) && wxFileExists(configuredPath)) {
        return configuredPath;
    }

    const wxUniChar pathSeparator = sigflow::platform::PathSeparator();

    wxString verilatorRoot;
    if (wxGetEnv("VERILATOR_ROOT", &verilatorRoot)) {
        const wxString rootCandidates[] = {
            verilatorRoot + pathSeparator + "bin" + pathSeparator +
                sigflow::platform::WithExecutableSuffix("verilator_bin_dbg"),
            verilatorRoot + pathSeparator + "bin" + pathSeparator +
                sigflow::platform::WithExecutableSuffix("verilator_bin"),
            verilatorRoot + pathSeparator + "bin" + pathSeparator +
                sigflow::platform::WithExecutableSuffix("verilator")
        };
        for (const auto& candidate : rootCandidates) {
            if (wxFileExists(candidate)) {
                return candidate;
            }
        }
    }

    const wxString verilatorBinDbg = sigflow::platform::WithExecutableSuffix("verilator_bin_dbg");
    const wxString bundledCandidates[] = {
        GetSoftwareDirectory() + pathSeparator + "tools" + pathSeparator + "verilator" + pathSeparator + "verilator-install" + pathSeparator + "bin" + pathSeparator + verilatorBinDbg,
        GetSoftwareDirectory() + pathSeparator + "tools" + pathSeparator + "verilator" + pathSeparator + "bin" + pathSeparator + verilatorBinDbg,
        m_projectRoot + pathSeparator + "tools" + pathSeparator + "verilator" + pathSeparator + "verilator-install" + pathSeparator + "bin" + pathSeparator + verilatorBinDbg,
        m_projectRoot + pathSeparator + "tools" + pathSeparator + "verilator" + pathSeparator + "bin" + pathSeparator + verilatorBinDbg
    };
    for (const auto& candidate : bundledCandidates) {
        if (wxFileExists(candidate)) {
            return candidate;
        }
    }

    // 仓库内置：从可执行文件所在目录逐级向上查找 tools\verilator\verilator-install\bin
    {
        wxFileName exeFile(wxStandardPaths::Get().GetExecutablePath());
        exeFile.SetFullName(wxEmptyString);
        wxString directory = exeFile.GetPath();
        for (int depth = 0; depth < 6 && !directory.IsEmpty(); ++depth) {
            const wxString binDirectory =
                directory + pathSeparator + "tools" + pathSeparator + "verilator" + pathSeparator + "verilator-install" + pathSeparator + "bin" + pathSeparator;
            const wxString repoCandidates[] = {
                binDirectory + sigflow::platform::WithExecutableSuffix("verilator_bin"),
                binDirectory + sigflow::platform::WithExecutableSuffix("verilator_bin_dbg"),
                binDirectory + sigflow::platform::WithExecutableSuffix("verilator"),
            };
            for (const auto& candidate : repoCandidates) {
                if (wxFileExists(candidate)) return candidate;
            }
            wxFileName parent(directory);
            parent.RemoveLastDir();
            const wxString parentPath = parent.GetPath();
            if (parentPath == directory) break;
            directory = parentPath;
        }
    }

#if defined(_WIN32)
    const wxString defaultCandidates[] = {
        "C:\\msys64\\mingw64\\bin\\verilator_bin.exe",
        "C:\\msys64\\usr\\bin\\verilator_bin.exe",
        "C:\\msys64\\mingw64\\bin\\verilator.exe",
        "C:\\msys64\\usr\\bin\\verilator.exe",
        "C:\\verilator\\bin\\verilator.exe",
        "C:\\Program Files\\verilator\\bin\\verilator.exe",
        "C:\\ProgramData\\chocolatey\\bin\\verilator.exe"
    };
    for (const auto& candidate : defaultCandidates) {
        if (wxFileExists(candidate)) {
            return candidate;
        }
    }
#endif

    // 尝试从 PATH 中查找（优先找 verilator_bin.exe，因为 MSYS2 的 verilator 是脚本）
    wxString pathEnv;
    if (wxGetEnv("PATH", &pathEnv)) {
        const std::vector<wxString> paths = sigflow::platform::SplitPathVariable(pathEnv);
        // 先找 verilator_bin.exe（MSYS2 实际可执行文件）
        for (const auto& p : paths) {
            wxString verilatorPath = p + pathSeparator + sigflow::platform::WithExecutableSuffix("verilator_bin");
            if (wxFileExists(verilatorPath)) {
                return verilatorPath;
            }
        }
        // 再找 verilator.exe
        for (const auto& p : paths) {
            wxString verilatorPath = p + pathSeparator + sigflow::platform::WithExecutableSuffix("verilator");
            if (wxFileExists(verilatorPath)) {
                return verilatorPath;
            }
        }
    }

    // 未找到时返回空串：调用方据此给出可诊断的错误，而不是拿一个不存在的名字去 CreateProcess。
    return wxEmptyString;
}

wxString SimulationEngine::FindVerilatorIncludePath() const
{
    // 辅助 lambda：检查目录是否包含 Verilator 核心文件
    auto isVerilatorDir = [](const wxString& dir) -> bool {
        return wxFileExists(dir + sigflow::platform::PathSeparator() + "verilated.h") ||
               wxFileExists(dir + sigflow::platform::PathSeparator() + "verilated_std_waiver.vlt");
    };

    wxString verilatorRoot;
    if (wxGetEnv("VERILATOR_ROOT", &verilatorRoot)) {
        // MSYS2 安装的 Verilator 文件在 share/verilator/include 下
        const wxString shareCandidates[] = {
            verilatorRoot + sigflow::platform::PathSeparator() + "share" + sigflow::platform::PathSeparator() + "verilator" + sigflow::platform::PathSeparator() + "include",
            verilatorRoot + sigflow::platform::PathSeparator() + "include"
        };
        for (const auto& candidate : shareCandidates) {
            if (wxDirExists(candidate) && isVerilatorDir(candidate)) {
                return candidate;
            }
        }
    }

    wxString verilatorPath = FindVerilatorPath();
    if (wxFileExists(verilatorPath)) {
        wxFileName prefix(verilatorPath);
        prefix.SetFullName(wxEmptyString);
        prefix.RemoveLastDir(); // bin
        const wxString candidates[] = {
            prefix.GetPath() + sigflow::platform::PathSeparator() + "share" + sigflow::platform::PathSeparator() + "verilator" + sigflow::platform::PathSeparator() + "include",
            prefix.GetPath() + sigflow::platform::PathSeparator() + "include"
        };
        for (const auto& candidate : candidates) {
            if (wxDirExists(candidate)) {
                return candidate;
            }
        }
    }

    const wxString defaultCandidates[] = {
        "C:\\msys64\\mingw64\\share\\verilator\\include",
        "C:\\msys64\\usr\\share\\verilator\\include",
        "C:\\verilator\\include",
        "C:\\Program Files\\verilator\\include"
    };
    for (const auto& candidate : defaultCandidates) {
        if (wxDirExists(candidate)) {
            return candidate;
        }
    }

    return wxEmptyString;
}

void SimulationEngine::ReportProgress(int percent, const wxString& status)
{
    // 简化输出，避免乱码
    char buf[256];
    snprintf(buf, sizeof(buf), "[%d%%] Progress update\n", percent);
    SIGFLOW_LOG(buf);
    
    // 安全调用回调
    if (m_progressCallback) {
        try {
            m_progressCallback(percent, status);
        } catch (...) {
            // 忽略回调错误
        }
    }
}

SimulationCompileResult SimulationEngine::Compile(const wxString& topModule, 
                                                  const std::vector<wxString>& verilogFiles)
{
    SIGFLOW_LOG("=== Compile Start ===\n");
    
    SimulationCompileResult result;
    m_currentTopModule = topModule;
    
    // 简化输出，避免任何可能的空指针问题
    SIGFLOW_LOG("Top module set\n");
    
    size_t count = verilogFiles.size();
    char buf[64];
    snprintf(buf, sizeof(buf), "File count: %zu\n", count);
    SIGFLOW_LOG(buf);

    SIGFLOW_LOG("Before ReportProgress\n");
    ReportProgress(0, wxT("开始编译仿真模型"));
    SIGFLOW_LOG("After ReportProgress\n");

    // 检查输入文件
    if (verilogFiles.empty()) {
        result.errorMessage = "没有提供Verilog文件";
        ReportProgress(0, result.errorMessage);
        return result;
    }

    SIGFLOW_LOG("Finding Verilator...\n");
    
    // 检查Verilator是否可用
    wxString verilatorPath = FindVerilatorPath();
    SIGFLOW_LOG("Got verilator path\n");
    
    if (verilatorPath.IsEmpty()) {
        SIGFLOW_LOG("Resolving from PATH...\n");
        // 尝试在PATH中查找
        wxString pathEnv;
        if (wxGetEnv(wxT("PATH"), &pathEnv)) {
            const std::vector<wxString> paths = sigflow::platform::SplitPathVariable(pathEnv);
            for (const auto& p : paths) {
                wxString testPath = p + sigflow::platform::PathSeparator() + sigflow::platform::WithExecutableSuffix("verilator_bin");
                if (wxFileExists(testPath)) {
                    verilatorPath = testPath;
                    SIGFLOW_LOG("Found in PATH\n");
                    break;
                }
            }
        }
    }
    
    SIGFLOW_LOG("Checking if verilator was found...\n");
    if (verilatorPath.IsEmpty()) {
        SIGFLOW_LOG("ERROR: Verilator not found\n");
        result.errorMessage = wxT("找不到 Verilator：请安装 Verilator 并加入 PATH，"
                                  "或设置环境变量 VERILATOR_BIN，"
                                  "或将 Verilator 放到 tools\\verilator 目录下。");
        ReportProgress(0, result.errorMessage);
        return result;
    }
    
    SIGFLOW_LOG("Verilator found\n");

    SIGFLOW_LOG("Getting cache directory...\n");
    // 创建缓存目录
    wxString cacheDir = GetCacheDirectory(topModule);
    wxString objDir = GetObjDirPath(topModule);
    
    // 转换为绝对路径
    wxFileName cacheDirFn(cacheDir);
    cacheDirFn.MakeAbsolute();
    cacheDir = cacheDirFn.GetFullPath();
    
    wxFileName objDirFn(objDir);
    objDirFn.MakeAbsolute();
    objDir = objDirFn.GetFullPath();
    
    SIGFLOW_LOG("Got cache dir\n");
    
    SIGFLOW_LOG("Creating directories...\n");
    if (!CreateDirectoryRecursive(objDir)) {
        SIGFLOW_LOG("ERROR: Failed to create directory\n");
        result.errorMessage = wxString::Format(wxT("无法创建缓存目录: %s"), cacheDir);
        ReportProgress(0, result.errorMessage);
        return result;
    }
    SIGFLOW_LOG("Directories created\n");

    // 清理旧的 obj_dir（避免不同版本 Verilator 生成的文件混在一起导致编译错误）
    SIGFLOW_LOG("Cleaning old obj_dir...\n");
    {
        std::error_code ec;
        std::filesystem::remove_all(objDir.ToStdString(), ec);
        std::filesystem::create_directories(objDir.ToStdString(), ec);
    }
    SIGFLOW_LOG("obj_dir cleaned\n");

    ReportProgress(10, wxString::Format(wxT("缓存目录: %s"), cacheDir));

    SIGFLOW_LOG("Calling RunVerilator...\n");
    // 步骤1: 运行Verilator生成C++代码
    wxString errMsg;
    if (!RunVerilator(topModule, verilogFiles, errMsg)) {
        SIGFLOW_LOG("RunVerilator failed\n");
        if (errMsg.IsEmpty()) {
            result.errorMessage = wxT("Verilator编译失败");
        } else {
            result.errorMessage = errMsg;
        }
        SIGFLOW_LOG("Returning error result\n");
        return result;
    }
    SIGFLOW_LOG("RunVerilator succeeded\n");

    // 步骤2: 编译生成DLL
    if (!CompileToDll(topModule, result.errorMessage)) {
        ReportProgress(0, result.errorMessage);
        return result;
    }

    // 成功
    result.success = true;
    result.cacheDir = cacheDir;
    result.dllPath = JoinPath(cacheDir, topModule + sigflow::platform::SharedLibrarySuffix());
    m_lastResult = result;

    ReportProgress(100, "编译完成!");
    return result;
}

bool SimulationEngine::RunVerilator(const wxString& topModule, 
                                    const std::vector<wxString>& verilogFiles,
                                    wxString& errorMsg)
{
    SIGFLOW_LOG("RunVerilator entered\n");
    ReportProgress(20, "正在生成C++代码(Verilator)...");

    wxString objDir = GetObjDirPath(topModule);
    wxString cacheDir = GetCacheDirectory(topModule);
    wxString verilatorPath = FindVerilatorPath();

    std::vector<std::pair<wxString, wxString>> verilatorEnvironment;
    {
        const wxString binDirectory = wxFileName(verilatorPath).GetPath();
        const wxString installRoot = wxFileName(binDirectory).GetPath();
        if (!installRoot.IsEmpty() &&
            wxFileName(binDirectory).GetFullName().IsSameAs("bin", false) &&
            wxFileExists(installRoot + sigflow::platform::PathSeparator() + "include" + sigflow::platform::PathSeparator() + "verilated.h")) {
            verilatorEnvironment.push_back({ "VERILATOR_ROOT", installRoot });
        }
    }

    SIGFLOW_LOG("Building command...\n");
    // 直接以 argv 方式调用 Verilator：路径转义交给 PlatformProcess 规范处理，
    // 避免 cmd.exe 对 \" 的错误解析（wxExecute 时代的遗留问题）。
    std::vector<wxString> verilatorArguments;
    verilatorArguments.push_back("-cc");
    verilatorArguments.push_back("-O0");  // 禁用优化，保留完整电路结构
    verilatorArguments.push_back("--Wno-DECLFILENAME");
    verilatorArguments.push_back("--Wno-TIMESCALEMOD");  // 忽略 timescale 不一致警告
    verilatorArguments.push_back("--timing");  // 支持时序控制（如 #1 延迟）
    verilatorArguments.push_back("--Mdir");
    verilatorArguments.push_back(objDir);
    verilatorArguments.push_back("--top-module");
    verilatorArguments.push_back(topModule);
    verilatorArguments.push_back("--trace");
    verilatorArguments.push_back("--trace-underscore");
    verilatorArguments.push_back("--trace-structs");
    for (const wxString& file : verilogFiles) {
        verilatorArguments.push_back(file);
    }

    wxString output, error;
    SIGFLOW_LOG("Executing command...\n");
    const PlatformProcessResult verilatorResult = RunTool(verilatorPath,
        verilatorArguments, m_projectRoot, 600, false, false, verilatorEnvironment);
    output = verilatorResult.output;
    error = verilatorResult.errorOutput;
    const int ret = verilatorResult.exitCode;
    SIGFLOW_LOG(("Command returned: " + std::to_string(ret) + "\n").c_str());

    if (!verilatorResult.started) {
        errorMsg = wxT("无法启动 Verilator 进程: ") + verilatorResult.errorMessage;
        return false;
    }
    if (verilatorResult.timedOut) {
        errorMsg = wxT("Verilator 执行超时（600秒上限）");
        return false;
    }
    if (ret != 0) {
        SIGFLOW_LOG("Command failed, setting error message...\n");
        errorMsg = wxString::Format(wxT("Verilator执行失败 (返回值: %d)"), ret);
        if (!output.IsEmpty()) {
            errorMsg += wxT("\n\n输出:\n") + output.Left(500);  // 限制长度
        }
        if (!error.IsEmpty()) {
            errorMsg += wxT("\n\n错误:\n") + error.Left(500);
        }
        SIGFLOW_LOG("Error message set\n");
        return false;
    }

    SIGFLOW_LOG("Command succeeded\n");
    ReportProgress(50, "C++代码生成完成");
    return true;
}

void SimulationEngine::CreateScTimeStub(const wxString& path)
{
    SIGFLOW_LOG("Creating sc_time_stub.cpp at: ");
    SIGFLOW_LOG(path.ToUTF8().data());
    SIGFLOW_LOG("\n");
    
    // C++20 模式下使用普通 C++ 链接，不加 extern "C"
    const char* stubContent = 
        "// Stub for sc_time_stamp function required by Verilator\n"
        "#include <cstdint>\n"
        "\n"
        "static uint64_t g_sim_time = 0;\n"
        "\n"
        "double sc_time_stamp() {\n"
        "    return static_cast<double>(g_sim_time);\n"
        "}\n"
        "\n"
        "void advance_sim_time(uint64_t delta) {\n"
        "    g_sim_time += delta;\n"
        "}\n";
    
    wxFile file(path, wxFile::write);
    if (file.IsOpened()) {
        file.Write(wxString::FromUTF8(stubContent));
        file.Close();
        SIGFLOW_LOG("sc_time_stub.cpp created successfully\n");
    } else {
        SIGFLOW_LOG("ERROR: Failed to create sc_time_stub.cpp\n");
    }
}

bool SimulationEngine::CompileToDll(const wxString& topModule, wxString& errorMsg)
{
    SIGFLOW_LOG("=== CompileToDll entered ===\n");
    ReportProgress(60, "正在编译DLL...");

    wxString cacheDir = GetCacheDirectory(topModule);
    wxString objDir = GetObjDirPath(topModule);
    
    // 转换为绝对路径
    wxFileName cacheDirFn(cacheDir);
    cacheDirFn.MakeAbsolute();
    cacheDir = cacheDirFn.GetFullPath();
    
    wxFileName objDirFn(objDir);
    objDirFn.MakeAbsolute();
    objDir = objDirFn.GetFullPath();
    
    wxString dllPath = JoinPath(cacheDir, topModule + sigflow::platform::SharedLibrarySuffix());
    
    SIGFLOW_LOG("Cache dir: ");
    SIGFLOW_LOG(cacheDir.ToUTF8().data());
    SIGFLOW_LOG("\n");
    SIGFLOW_LOG("Obj dir: ");
    SIGFLOW_LOG(objDir.ToUTF8().data());
    SIGFLOW_LOG("\n");
    SIGFLOW_LOG("DLL path: ");
    SIGFLOW_LOG(dllPath.ToUTF8().data());
    SIGFLOW_LOG("\n");
    
    // 确保项目根目录是绝对路径
    wxFileName projectRootFn(m_projectRoot);
    projectRootFn.MakeAbsolute();
    wxString projectRoot = projectRootFn.GetFullPath();
    
    SIGFLOW_LOG("Project root: ");
    SIGFLOW_LOG(projectRoot.ToUTF8().data());
    SIGFLOW_LOG("\n");
    
    SIGFLOW_LOG("Preparing shared-library compile...\n");
    
    // 直接使用软件目录下的 sc_time_stub.cpp，不复制
    wxString softwareDir = GetSoftwareDirectory();
    wxString stubPath = JoinPath(JoinPath(JoinPath(softwareDir, "main"), "Simulation"), "sc_time_stub.cpp");
    
    SIGFLOW_LOG("Stub path: ");
    SIGFLOW_LOG(stubPath.ToUTF8().data());
    SIGFLOW_LOG("\n");
    
    // 检查 stub 文件是否存在
    if (!wxFileExists(stubPath)) {
        SIGFLOW_LOG("Stub file not found, trying to create...\n");
        // 尝试在缓存目录创建
        stubPath = JoinPath(cacheDir, "sc_time_stub.cpp");
        CreateScTimeStub(stubPath);
        if (!wxFileExists(stubPath)) {
            errorMsg = wxT("无法创建 sc_time_stub.cpp");
            return false;
        }
    }
    
    wxString verilatorIncludePath = FindVerilatorIncludePath();
    
    // 初始化编译状态（加锁保护）
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_isCompiling = true;
        m_dllCompileSuccess = false;
        m_lastCompileLog.Clear();
    }

    if (SimCancelRequested(m_jobProjectPath, m_jobId)) {
        errorMsg = wxT("用户取消仿真作业");
        std::lock_guard<std::mutex> lock(m_mutex);
        m_isCompiling = false;
        return false;
    }

    sigflow::platform::SimToolchain::SharedLibRequest toolchainRequest;
    toolchainRequest.cacheDir = cacheDir.ToUTF8().data();
    toolchainRequest.objDir = objDir.ToUTF8().data();
    toolchainRequest.outputPath = dllPath.ToUTF8().data();
    toolchainRequest.stubSourcePath = stubPath.ToUTF8().data();
    toolchainRequest.verilatorIncludeDir = verilatorIncludePath.ToUTF8().data();
    toolchainRequest.workingDirectory = projectRoot.ToUTF8().data();
    toolchainRequest.onStarted = [this](void* handle) {
        if (!m_jobId.IsEmpty()) {
            JobService::RegisterProcess(m_jobProjectPath, m_jobId, handle);
        }
    };
    toolchainRequest.onFinished = [this]() {
        if (!m_jobId.IsEmpty()) {
            JobService::UnregisterProcess(m_jobProjectPath, m_jobId);
        }
    };

    PlatformOutputCallback sink = [this](const wxString& chunk, bool isError) {
        CompileOutputCallback callback;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_lastCompileLog += chunk;
            callback = m_outputCallback;
        }
        if (callback) callback(chunk, isError);
    };

    std::string toolchainError;
    const bool compileSucceeded = sigflow::platform::SimToolchain::CompileSharedLibrary(
        toolchainRequest, sink, toolchainError);

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_isCompiling = false;
        m_dllCompileSuccess = compileSucceeded;
    }

    if (!compileSucceeded) {
        if (SimCancelRequested(m_jobProjectPath, m_jobId)) {
            errorMsg = wxT("用户取消仿真作业");
        } else {
            errorMsg = wxString::FromUTF8(toolchainError.c_str());
        }
        wxString log;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            log = m_lastCompileLog;
        }
        if (!log.IsEmpty()) {
            errorMsg += wxT("\n\n编译日志:\n") + log.Left(2000);
        }
        return false;
    }

    SIGFLOW_LOG("DLL compile success\n");
    ReportProgress(90, wxString::Format(wxT("DLL生成成功: %s"), dllPath));
    return true;
}

bool SimulationEngine::IsCompiled(const wxString& topModule) const
{
    wxString dllPath = JoinPath(GetCacheDirectory(topModule), topModule + sigflow::platform::SharedLibrarySuffix());
    return wxFileExists(dllPath);
}

bool SimulationEngine::CleanCache(const wxString& topModule)
{
    wxString cacheDir = GetCacheDirectory(topModule);
    
    try {
        fs::path p(fs::u8path(cacheDir.ToUTF8().data()));
        if (fs::exists(p)) {
            fs::remove_all(p);
        }
        return true;
    }
    catch (const std::exception& e) {
        SIGFLOW_LOG(("CleanCache error: " + std::string(e.what()) + "\n").c_str());
        return false;
    }
}

SimulationRunResult SimulationEngine::RunSimulation(const wxString& outputVcdPath)
{
    SimulationRunResult result;

    SIGFLOW_LOG("=== RunSimulation Start ===\n");

    // 1. 检查是否有可用的 DLL（支持跨会话：直接检查文件系统）
    wxString topModule = m_currentTopModule;
    if (topModule.IsEmpty()) {
        result.errorMessage = wxT("未指定顶层模块名，请先编译");
        return result;
    }

    wxString cacheDir = GetCacheDirectory(topModule);
    wxFileName cacheDirFn(cacheDir);
    cacheDirFn.MakeAbsolute();
    cacheDir = cacheDirFn.GetFullPath();

    wxString dllPath = JoinPath(cacheDir, topModule + sigflow::platform::SharedLibrarySuffix());
    if (!wxFileExists(dllPath)) {
        result.errorMessage = wxT("没有可用的编译结果，请先编译\n找不到: ") + dllPath;
        return result;
    }

    SIGFLOW_LOG(("DLL found: " + dllPath.ToStdString() + "\n").c_str());
    ReportProgress(10, wxT("找到编译结果，准备仿真..."));

    // 2. 模糊匹配 Testbench 文件
    wxString testbenchPath = FindTestbenchFile(m_projectRoot);
    if (testbenchPath.IsEmpty()) {
        result.errorMessage = wxT("找不到 Testbench 文件\n")
            wxT("请在项目 src 目录下创建名为 test_bench.v / testbench.v / tb_*.v 等文件");
        return result;
    }
    SIGFLOW_LOG(("Testbench found: " + testbenchPath.ToStdString() + "\n").c_str());
    ReportProgress(20, wxString::Format(wxT("找到 Testbench: %s"), testbenchPath));

    // 3. 生成 sim_main.cpp 并编译为 sim_runner.exe
    wxString compileError;
    if (!CompileSimRunner(topModule, testbenchPath, compileError)) {
        result.errorMessage = wxT("仿真编译失败: ") + compileError;
        return result;
    }
    ReportProgress(70, wxT("sim_runner.exe 编译完成"));

    // 4. 运行 sim_runner.exe 生成 VCD
    wxString runError;
    if (!ExecuteSimRunner(topModule, runError)) {
        result.errorMessage = wxT("仿真运行失败: ") + runError;
        return result;
    }

    // 5. 检查 VCD 输出
    wxString vcdPath = JoinPath(JoinPath(cacheDir, "waveform"), "wave.vcd");
    if (!wxFileExists(vcdPath)) {
        result.errorMessage = wxT("仿真完成但未生成波形文件");
        return result;
    }

    // 如果用户指定了输出路径，复制 VCD 过去
    if (!outputVcdPath.IsEmpty() && outputVcdPath != vcdPath) {
        wxCopyFile(vcdPath, outputVcdPath);
        result.vcdPath = outputVcdPath;
    } else {
        result.vcdPath = vcdPath;
    }

    result.success = true;
    ReportProgress(100, wxT("仿真完成!"));
    SIGFLOW_LOG("=== RunSimulation Success ===\n");
    return result;
}

wxString SimulationEngine::FindTestbenchFile(const wxString& projectRoot) const
{
    wxString srcDir = JoinPath(projectRoot, "src");
    if (!wxDir::Exists(srcDir)) {
        SIGFLOW_LOG("src directory not found\n");
        return wxEmptyString;
    }

    wxDir dir;
    if (!dir.Open(srcDir)) return wxEmptyString;

    // 模糊匹配模式（不区分大小写）
    // 优先级从高到低
    const std::vector<std::string> patterns = {
        "test_bench", "testbench", "Test_Bench", "TestBench",
        "tb_", "TB_", "Tb_",
        "_tb.", "_TB.",
        "stimulus", "Stimulus"
    };

    wxString filename;
    std::vector<wxString> candidates;

    bool hasFile = dir.GetFirst(&filename, "*.v", wxDIR_FILES);
    while (hasFile) {
        candidates.push_back(filename);
        hasFile = dir.GetNext(&filename);
    }
    // 也搜索 .sv 文件
    hasFile = dir.GetFirst(&filename, "*.sv", wxDIR_FILES);
    while (hasFile) {
        candidates.push_back(filename);
        hasFile = dir.GetNext(&filename);
    }

    // 按优先级匹配
    for (const auto& pattern : patterns) {
        for (const auto& candidate : candidates) {
            wxString lower = candidate.Lower();
            if (lower.Contains(wxString(pattern).Lower())) {
                return JoinPath(srcDir, candidate);
            }
        }
    }

    SIGFLOW_LOG("No testbench file found by fuzzy matching\n");
    return wxEmptyString;
}

bool SimulationEngine::CompileSimRunner(const wxString& topModule, const wxString& testbenchPath,
                                        wxString& errorMsg)
{
    SIGFLOW_LOG("=== CompileSimRunner Start ===\n");
    ReportProgress(30, wxT("解析 Testbench..."));

    wxString cacheDir = GetCacheDirectory(topModule);
    wxFileName cacheDirFn(cacheDir);
    cacheDirFn.MakeAbsolute();
    cacheDir = cacheDirFn.GetFullPath();

    wxString objDir = GetObjDirPath(topModule);
    wxFileName objDirFn(objDir);
    objDirFn.MakeAbsolute();
    objDir = objDirFn.GetFullPath();

    // 1. 解析 Testbench
    StimulusParser parser;
    TestbenchInfo tbInfo;
    if (!parser.Parse(testbenchPath, tbInfo)) {
        errorMsg = wxT("Testbench 解析失败: ") + parser.GetLastError();
        return false;
    }

    SIGFLOW_LOG(("Parsed testbench: module=" + tbInfo.moduleName
        + " top=" + tbInfo.topModuleName + "\n").c_str());

    // 2. 生成时间线
    ReportProgress(40, wxT("生成仿真时间线..."));
    TimelineGenerator tlGen;
    Timeline timeline = tlGen.Generate(tbInfo, topModule);

    SIGFLOW_LOG(("Timeline: events=" + std::to_string(timeline.events.size())
        + " maxTime=" + std::to_string(timeline.maxSimTime) + "\n").c_str());

    // 3. 生成 sim_main.cpp
    ReportProgress(45, wxT("生成 sim_main.cpp..."));
    wxString simMainPath = JoinPath(cacheDir, "sim_main.cpp");
    SimMainGenerator mainGen;
    if (!mainGen.Generate(timeline, simMainPath)) {
        errorMsg = wxT("sim_main.cpp 生成失败: ") + mainGen.GetLastError();
        return false;
    }

    SIGFLOW_LOG(("sim_main.cpp generated at: " + simMainPath.ToStdString() + "\n").c_str());

    // 4. 创建 waveform 目录
    wxString waveDir = JoinPath(cacheDir, "waveform");
    CreateDirectoryRecursive(waveDir);

    // 5. 编译 sim_runner.exe
    ReportProgress(50, wxT("编译 sim_runner.exe..."));

    wxString exePath = JoinPath(cacheDir, wxString("sim_runner") + sigflow::platform::ExecutableSuffix());
    wxString verilatorIncludePath = FindVerilatorIncludePath();

    // sc_time_stub
    wxString stubPath = JoinPath(JoinPath(JoinPath(GetSoftwareDirectory(), "main"), "Simulation"), "sc_time_stub.cpp");
    if (!wxFileExists(stubPath)) {
        stubPath = JoinPath(cacheDir, "sc_time_stub.cpp");
        CreateScTimeStub(stubPath);
    }

    // 确保项目根目录是绝对路径
    wxFileName projectRootFn(m_projectRoot);
    projectRootFn.MakeAbsolute();
    wxString projectRoot = projectRootFn.GetFullPath();

    if (SimCancelRequested(m_jobProjectPath, m_jobId)) {
        errorMsg = wxT("用户取消仿真作业");
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_lastCompileLog.Clear();
    }

    sigflow::platform::SimToolchain::ExecutableRequest toolchainRequest;
    toolchainRequest.cacheDir = cacheDir.ToUTF8().data();
    toolchainRequest.objDir = objDir.ToUTF8().data();
    toolchainRequest.outputPath = exePath.ToUTF8().data();
    toolchainRequest.mainSourcePath = simMainPath.ToUTF8().data();
    toolchainRequest.stubSourcePath = stubPath.ToUTF8().data();
    toolchainRequest.verilatorIncludeDir = verilatorIncludePath.ToUTF8().data();
    toolchainRequest.workingDirectory = projectRoot.ToUTF8().data();
    toolchainRequest.onStarted = [this](void* handle) {
        if (!m_jobId.IsEmpty()) {
            JobService::RegisterProcess(m_jobProjectPath, m_jobId, handle);
        }
    };
    toolchainRequest.onFinished = [this]() {
        if (!m_jobId.IsEmpty()) {
            JobService::UnregisterProcess(m_jobProjectPath, m_jobId);
        }
    };

    PlatformOutputCallback sink = [this](const wxString& chunk, bool isError) {
        CompileOutputCallback callback;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_lastCompileLog += chunk;
            callback = m_outputCallback;
        }
        if (callback) callback(chunk, isError);
    };

    std::string toolchainError;
    const bool compileSucceeded = sigflow::platform::SimToolchain::CompileExecutable(
        toolchainRequest, sink, toolchainError);

    if (!compileSucceeded) {
        errorMsg = wxString::FromUTF8(toolchainError.c_str());
        wxString log;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            log = m_lastCompileLog;
        }
        if (!log.IsEmpty()) {
            errorMsg += wxT("\n\n编译日志:\n") + log.Left(2000);
        }
        return false;
    }

    SIGFLOW_LOG("=== CompileSimRunner Success ===\n");
    return true;
}

bool SimulationEngine::ExecuteSimRunner(const wxString& topModule, wxString& errorMsg)
{
    SIGFLOW_LOG("=== ExecuteSimRunner Start ===\n");
    ReportProgress(80, wxT("运行仿真..."));

    wxString cacheDir = GetCacheDirectory(topModule);
    wxFileName cacheDirFn(cacheDir);
    cacheDirFn.MakeAbsolute();
    cacheDir = cacheDirFn.GetFullPath();

    wxString exePath = JoinPath(cacheDir, wxString("sim_runner") + sigflow::platform::ExecutableSuffix());
    if (!wxFileExists(exePath)) {
        errorMsg = wxT("找不到 sim_runner.exe");
        return false;
    }

    // 确保 waveform 目录存在
    wxString waveDir = JoinPath(cacheDir, "waveform");
    CreateDirectoryRecursive(waveDir);

    if (SimCancelRequested(m_jobProjectPath, m_jobId)) {
        errorMsg = wxT("用户取消仿真作业");
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_lastCompileLog.Clear();
    }

    // 运行 sim_runner.exe（工作目录设为 cacheDir，因为 VCD 路径是相对的）；60s 上限沿用旧行为
    const PlatformProcessResult runResult = RunTool(exePath, {}, cacheDir, 60, true);

    if (SimCancelRequested(m_jobProjectPath, m_jobId)) {
        errorMsg = wxT("用户取消仿真作业");
        return false;
    }
    if (!runResult.started) {
        errorMsg = wxT("启动 sim_runner.exe 失败: ") + runResult.errorMessage;
        return false;
    }
    if (runResult.timedOut) {
        errorMsg = wxT("仿真运行超时（60秒）");
        return false;
    }
    if (runResult.exitCode != 0) {
        errorMsg = wxString::Format(wxT("仿真运行失败 (错误码: %d)"), runResult.exitCode);
        if (!runResult.output.IsEmpty()) {
            errorMsg += wxT("\n\n运行日志:\n") + runResult.output.Left(2000);
        }
        return false;
    }

    SIGFLOW_LOG("=== ExecuteSimRunner Success ===\n");
    ReportProgress(95, wxT("波形文件生成完成"));
    return true;
}

bool SimulationEngine::IsCompiling() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_isCompiling;
}

void SimulationEngine::CancelCompile()
{
    // 进程句柄由 RunTool 登记在 JobService；取消即终止整棵进程树。
    if (m_jobId.IsEmpty()) return;
    wxString ignored;
    JobService().Cancel(m_jobProjectPath, m_jobId, "User cancelled simulation.", ignored);
}

// 获取软件自身所在目录（用于找到 sc_time_stub.cpp 等工具文件）
wxString SimulationEngine::GetSoftwareDirectory() const
{
    // 方法1: 尝试从环境变量获取
    wxString envPath;
    if (wxGetEnv("SIGFLOW_ROOT", &envPath) && !envPath.IsEmpty()) {
        return envPath;
    }
    
    // 方法2: 基于可执行文件路径推导
    wxString exePath = wxStandardPaths::Get().GetExecutablePath();
    wxFileName exeDir(exePath);
    wxString path = exeDir.GetPath();
    
    // 输出调试信息
    SIGFLOW_LOG("Executable path: ");
    SIGFLOW_LOG(path.ToUTF8().data());
    SIGFLOW_LOG("\n");
    
    // 如果在 x64/Release 或 x64/Debug 下，向上两级
    if (path.Lower().Contains("x64")) {
        exeDir.RemoveLastDir();  // 去掉 Release/Debug
        exeDir.RemoveLastDir();  // 去掉 x64
        path = exeDir.GetPath();
    }
    
    // 检查是否在 main 目录下
    if (path.EndsWith("main") || path.EndsWith("main\\")) {
        // 已经在 main 目录，软件根目录是上级
        exeDir.RemoveLastDir();
    }
    
    wxString result = exeDir.GetPath();
    SIGFLOW_LOG("Software directory: ");
    SIGFLOW_LOG(result.ToUTF8().data());
    SIGFLOW_LOG("\n");
    
    return result;
}
