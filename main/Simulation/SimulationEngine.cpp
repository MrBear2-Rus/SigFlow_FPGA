#include "SimulationEngine.h"
#include "ProcessRunner.h"
#include "StimulusParser.h"
#include "TimelineGenerator.h"
#include "SimMainGenerator.h"
#include <wx/process.h>
#include <wx/txtstrm.h>
#include <wx/stdpaths.h>
#include <wx/filename.h>
#include <wx/dir.h>
#include <filesystem>
#include <fstream>
#include <windows.h>
#include <wx/timer.h>

namespace fs = std::filesystem;

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
    return GetCacheDirectory(topModule) + "\\obj_dir";
}

bool SimulationEngine::CreateDirectoryRecursive(const wxString& path)
{
    try {
        fs::path p(path.ToStdString());
        fs::create_directories(p);
        return true;
    }
    catch (const std::exception& e) {
        wxLogError("创建目录失败: %s - %s", path, e.what());
        return false;
    }
}

int SimulationEngine::ExecuteCommand(const wxString& cmd, wxString& output, wxString& error)
{
    OutputDebugStringA("ExecuteCommand entered\n");
    
    // 使用 wxArrayString 版本的 wxExecute，更稳定
    wxArrayString outputArr, errorArr;
    
    OutputDebugStringA("Calling wxExecute...\n");
    int exitCode = wxExecute(cmd, outputArr, errorArr, wxEXEC_SYNC | wxEXEC_HIDE_CONSOLE);
    OutputDebugStringA(("wxExecute returned: " + std::to_string(exitCode) + "\n").c_str());
    
    OutputDebugStringA("Merging output...\n");
    // 合并输出（限制行数避免过长）
    int lineCount = 0;
    for (const auto& line : outputArr) {
        if (lineCount < 50) {  // 只取前50行
            output += line + "\n";
            lineCount++;
        }
    }
    for (const auto& line : errorArr) {
        if (error.Length() < 2000) {  // 限制错误长度
            error += line + "\n";
        }
    }
    OutputDebugStringA("ExecuteCommand done\n");
    
    return exitCode;
}

wxString SimulationEngine::FindVerilatorPath() const
{
    wxString configuredPath;
    if (wxGetEnv("VERILATOR_BIN", &configuredPath) && wxFileExists(configuredPath)) {
        return configuredPath;
    }

    wxString verilatorRoot;
    if (wxGetEnv("VERILATOR_ROOT", &verilatorRoot)) {
        const wxString rootCandidates[] = {
            verilatorRoot + "\\bin\\verilator_bin.exe",
            verilatorRoot + "\\bin\\verilator.exe"
        };
        for (const auto& candidate : rootCandidates) {
            if (wxFileExists(candidate)) {
                return candidate;
            }
        }
    }

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

    // 尝试从 PATH 中查找（优先找 verilator_bin.exe，因为 MSYS2 的 verilator 是脚本）
    wxString pathEnv;
    if (wxGetEnv("PATH", &pathEnv)) {
        wxArrayString paths = wxSplit(pathEnv, ';');
        // 先找 verilator_bin.exe（MSYS2 实际可执行文件）
        for (const auto& p : paths) {
            wxString verilatorPath = p + "\\verilator_bin.exe";
            if (wxFileExists(verilatorPath)) {
                return verilatorPath;
            }
        }
        // 再找 verilator.exe
        for (const auto& p : paths) {
            wxString verilatorPath = p + "\\verilator.exe";
            if (wxFileExists(verilatorPath)) {
                return verilatorPath;
            }
        }
    }

    // 默认返回 verilator_bin（优先使用 MSYS2 的版本）
    return "verilator_bin";
}

wxString SimulationEngine::FindVerilatorIncludePath() const
{
    wxString verilatorRoot;
    if (wxGetEnv("VERILATOR_ROOT", &verilatorRoot)) {
        wxString includePath = verilatorRoot + "\\include";
        if (wxDirExists(includePath)) {
            return includePath;
        }
    }

    wxString verilatorPath = FindVerilatorPath();
    if (wxFileExists(verilatorPath)) {
        wxFileName prefix(verilatorPath);
        prefix.SetFullName(wxEmptyString);
        prefix.RemoveLastDir(); // bin
        const wxString candidates[] = {
            prefix.GetPath() + "\\share\\verilator\\include",
            prefix.GetPath() + "\\include"
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

wxString SimulationEngine::FindVCVarsPath() const
{
    // 按版本从新到旧搜索，支持 VS2026/2025/2022/2019
    const char* vsVersions[] = { "2026", "2025", "2022", "2019" };
    const char* editions[] = { "Community", "Professional", "Enterprise", "BuildTools" };
    const char* programDirs[] = {
        "C:\\Program Files\\Microsoft Visual Studio",
        "C:\\Program Files (x86)\\Microsoft Visual Studio"
    };

    for (const auto& ver : vsVersions) {
        for (const auto& progDir : programDirs) {
            for (const auto& edition : editions) {
                wxString path = wxString::Format("%s\\%s\\%s\\VC\\Auxiliary\\Build\\vcvars64.bat",
                                                 progDir, ver, edition);
                if (wxFileExists(path)) {
                    OutputDebugStringA(("Found vcvars: " + path.ToStdString() + "\n").c_str());
                    return path;
                }
            }
        }
    }

    // 最后尝试 vswhere.exe 自动定位（VS 2017+ 附带）
    wxString vswhere = "C:\\Program Files (x86)\\Microsoft Visual Studio\\Installer\\vswhere.exe";
    if (wxFileExists(vswhere)) {
        wxString cmd = "\"" + vswhere + "\" -latest -property installationPath";
        wxArrayString outputArr;
        if (wxExecute(cmd, outputArr, wxEXEC_SYNC | wxEXEC_HIDE_CONSOLE) == 0 && !outputArr.IsEmpty()) {
            wxString installPath = outputArr[0].Trim();
            wxString vcvars = installPath + "\\VC\\Auxiliary\\Build\\vcvars64.bat";
            if (wxFileExists(vcvars)) {
                return vcvars;
            }
        }
    }

    return wxEmptyString;
}

void SimulationEngine::ReportProgress(int percent, const wxString& status)
{
    // 简化输出，避免乱码
    char buf[256];
    sprintf_s(buf, "[%d%%] Progress update\n", percent);
    OutputDebugStringA(buf);
    
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
    OutputDebugStringA("=== Compile Start ===\n");
    
    SimulationCompileResult result;
    m_currentTopModule = topModule;
    
    // 简化输出，避免任何可能的空指针问题
    OutputDebugStringA("Top module set\n");
    
    size_t count = verilogFiles.size();
    char buf[64];
    sprintf_s(buf, "File count: %zu\n", count);
    OutputDebugStringA(buf);
    m_currentTopModule = topModule;

    OutputDebugStringA("Before ReportProgress\n");
    ReportProgress(0, wxT("开始编译仿真模型"));
    OutputDebugStringA("After ReportProgress\n");

    // 检查输入文件
    if (verilogFiles.empty()) {
        result.errorMessage = "没有提供Verilog文件";
        ReportProgress(0, result.errorMessage);
        return result;
    }

    OutputDebugStringA("Finding Verilator...\n");
    
    // 检查Verilator是否可用
    wxString verilatorPath = FindVerilatorPath();
    OutputDebugStringA("Got verilator path\n");
    
    if (verilatorPath.IsEmpty() || verilatorPath == wxT("verilator_bin")) {
        OutputDebugStringA("Resolving from PATH...\n");
        // 尝试在PATH中查找
        wxString pathEnv;
        if (wxGetEnv(wxT("PATH"), &pathEnv)) {
            wxArrayString paths = wxSplit(pathEnv, ';');
            for (const auto& p : paths) {
                wxString testPath = p + wxT("\\verilator_bin.exe");
                if (wxFileExists(testPath)) {
                    verilatorPath = testPath;
                    OutputDebugStringA("Found in PATH\n");
                    break;
                }
            }
        }
    }
    
    OutputDebugStringA("Checking if verilator was found...\n");
    if (verilatorPath.IsEmpty()) {
        OutputDebugStringA("ERROR: Verilator not found\n");
        result.errorMessage = wxT("找不到 Verilator，请确保已安装并添加到 PATH");
        ReportProgress(0, result.errorMessage);
        return result;
    }
    
    OutputDebugStringA("Verilator found\n");

    OutputDebugStringA("Getting cache directory...\n");
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
    
    OutputDebugStringA("Got cache dir\n");
    
    OutputDebugStringA("Creating directories...\n");
    if (!CreateDirectoryRecursive(objDir)) {
        OutputDebugStringA("ERROR: Failed to create directory\n");
        result.errorMessage = wxString::Format("无法创建缓存目录: %s", cacheDir);
        ReportProgress(0, result.errorMessage);
        return result;
    }
    OutputDebugStringA("Directories created\n");

    // 清理旧的 obj_dir（避免不同版本 Verilator 生成的文件混在一起导致编译错误）
    OutputDebugStringA("Cleaning old obj_dir...\n");
    {
        std::error_code ec;
        std::filesystem::remove_all(objDir.ToStdString(), ec);
        std::filesystem::create_directories(objDir.ToStdString(), ec);
    }
    OutputDebugStringA("obj_dir cleaned\n");

    ReportProgress(10, wxString::Format("缓存目录: %s", cacheDir));

    OutputDebugStringA("Calling RunVerilator...\n");
    // 步骤1: 运行Verilator生成C++代码
    wxString errMsg;
    if (!RunVerilator(topModule, verilogFiles, errMsg)) {
        OutputDebugStringA("RunVerilator failed\n");
        if (errMsg.IsEmpty()) {
            result.errorMessage = wxT("Verilator编译失败");
        } else {
            result.errorMessage = errMsg;
        }
        OutputDebugStringA("Returning error result\n");
        return result;
    }
    OutputDebugStringA("RunVerilator succeeded\n");

    // 步骤2: 编译生成DLL
    if (!CompileToDll(topModule, result.errorMessage)) {
        ReportProgress(0, result.errorMessage);
        return result;
    }

    // 成功
    result.success = true;
    result.cacheDir = cacheDir;
    result.dllPath = cacheDir + "\\" + topModule + ".dll";
    m_lastResult = result;

    ReportProgress(100, "编译完成!");
    return result;
}

bool SimulationEngine::RunVerilator(const wxString& topModule, 
                                    const std::vector<wxString>& verilogFiles,
                                    wxString& errorMsg)
{
    OutputDebugStringA("RunVerilator entered\n");
    ReportProgress(20, "正在生成C++代码(Verilator)...");

    wxString objDir = GetObjDirPath(topModule);
    wxString cacheDir = GetCacheDirectory(topModule);
    wxString verilatorPath = FindVerilatorPath();

    OutputDebugStringA("Building command...\n");
    // 构建命令
    // 注意：去掉 -Wall，避免把警告当作错误
    // 添加 --Wno-DECLFILENAME 忽略文件名不匹配警告
    // 注意：Verilator 5.x 不支持 --shared，只生成 C++ 代码
    wxString cmd = "\"" + verilatorPath + "\"";
    cmd += " -cc";
    cmd += " -O0";  // 禁用优化，保留完整电路结构
    cmd += " --Wno-DECLFILENAME";
    cmd += " --Wno-TIMESCALEMOD";  // 忽略 timescale 不一致警告
    cmd += " --timing";  // 支持时序控制（如 #1 延迟）
    cmd += " --Mdir \"" + objDir + "\"";
    cmd += " --top-module \"" + topModule + "\"";
    cmd += " --trace --trace-underscore --trace-structs";  // 波形跟踪接口
    // 只生成 C++，不编译可执行文件（--exe 和 --build 也不需要）
    
    // 添加所有Verilog文件
    for (const auto& file : verilogFiles) {
        cmd += " \"" + file + "\"";
    }

    OutputDebugStringA(("Command: " + std::string(cmd.ToUTF8()) + "\n").c_str());

    wxString output, error;
    OutputDebugStringA("Executing command...\n");
    int ret = ExecuteCommand(cmd, output, error);
    OutputDebugStringA(("Command returned: " + std::to_string(ret) + "\n").c_str());

    if (ret != 0) {
        OutputDebugStringA("Command failed, setting error message...\n");
        errorMsg = wxString::Format(wxT("Verilator执行失败 (返回值: %d)"), ret);
        if (!output.IsEmpty()) {
            errorMsg += wxT("\n\n输出:\n") + output.Left(500);  // 限制长度
        }
        if (!error.IsEmpty()) {
            errorMsg += wxT("\n\n错误:\n") + error.Left(500);
        }
        OutputDebugStringA("Error message set\n");
        return false;
    }

    OutputDebugStringA("Command succeeded\n");
    ReportProgress(50, "C++代码生成完成");
    return true;
}

void SimulationEngine::CreateScTimeStub(const wxString& path)
{
    OutputDebugStringA("Creating sc_time_stub.cpp at: ");
    OutputDebugStringA(path.ToUTF8());
    OutputDebugStringA("\n");
    
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
        OutputDebugStringA("sc_time_stub.cpp created successfully\n");
    } else {
        OutputDebugStringA("ERROR: Failed to create sc_time_stub.cpp\n");
    }
}

bool SimulationEngine::CompileToDll(const wxString& topModule, wxString& errorMsg)
{
    OutputDebugStringA("=== CompileToDll entered ===\n");
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
    
    wxString dllPath = cacheDir + "\\" + topModule + ".dll";
    
    OutputDebugStringA("Cache dir: ");
    OutputDebugStringA(cacheDir.ToUTF8());
    OutputDebugStringA("\n");
    OutputDebugStringA("Obj dir: ");
    OutputDebugStringA(objDir.ToUTF8());
    OutputDebugStringA("\n");
    OutputDebugStringA("DLL path: ");
    OutputDebugStringA(dllPath.ToUTF8());
    OutputDebugStringA("\n");
    
    // 确保项目根目录是绝对路径
    wxFileName projectRootFn(m_projectRoot);
    projectRootFn.MakeAbsolute();
    wxString projectRoot = projectRootFn.GetFullPath();
    
    OutputDebugStringA("Project root: ");
    OutputDebugStringA(projectRoot.ToUTF8());
    OutputDebugStringA("\n");
    
    // 查找 Visual Studio
    OutputDebugStringA("Finding vcvars...\n");
    wxString vcvarsPath = FindVCVarsPath();
    if (vcvarsPath.IsEmpty()) {
        OutputDebugStringA("ERROR: Visual Studio not found\n");
        errorMsg = wxT("找不到 Visual Studio，请安装 VS 2022 或更高版本");
        return false;
    }
    OutputDebugStringA("Found vcvars: ");
    OutputDebugStringA(vcvarsPath.ToUTF8());
    OutputDebugStringA("\n");
    
    // 直接使用软件目录下的 sc_time_stub.cpp，不复制
    wxString softwareDir = GetSoftwareDirectory();
    wxString stubPath = softwareDir + "\\main\\Simulation\\sc_time_stub.cpp";
    
    OutputDebugStringA("Stub path: ");
    OutputDebugStringA(stubPath.ToUTF8());
    OutputDebugStringA("\n");
    
    // 检查 stub 文件是否存在
    if (!wxFileExists(stubPath)) {
        OutputDebugStringA("Stub file not found, trying to create...\n");
        // 尝试在缓存目录创建
        stubPath = cacheDir + "\\sc_time_stub.cpp";
        CreateScTimeStub(stubPath);
        if (!wxFileExists(stubPath)) {
            errorMsg = wxT("无法创建 sc_time_stub.cpp");
            return false;
        }
    }
    
    // 创建临时批处理文件来执行编译
    OutputDebugStringA("Creating temp batch file...\n");
    wxString batchPath = cacheDir + "\\compile_dll.bat";
    wxString verilatorIncludePath = FindVerilatorIncludePath();
    if (verilatorIncludePath.IsEmpty()) {
        errorMsg = wxT("无法定位 Verilator 运行时目录；请设置 VERILATOR_ROOT 或 VERILATOR_BIN。");
        return false;
    }
    {
        wxFile batchFile(batchPath, wxFile::write);
        if (!batchFile.IsOpened()) {
            errorMsg = wxT("无法创建编译脚本");
            return false;
        }
        
        // 修正路径：确保没有双反斜杠，/Fo路径不以反斜杠结尾
        wxString safeStubPath = stubPath;
        safeStubPath.Replace("\\\\", "\\");  // 先去除双反斜杠
        safeStubPath.Replace("/", "\\");       // 再统一为 Windows 反斜杠
        
        wxString batchContent;
        batchContent += "@echo off\n";
        batchContent += "chcp 65001 >nul\n";
        batchContent += "call \"" + vcvarsPath + "\"\n";
        batchContent += "if %errorLevel% neq 0 exit /b %errorLevel%\n";
        batchContent += "cd /d \"" + objDir + "\"\n";               // 切到 objDir，避免空格路径问题
        batchContent += "cl /LD /O2 /MD /EHsc /W3 /std:c++20 ";     // C++20 标准协程
        batchContent += "/Fe\"" + dllPath + "\" ";
        // /Fo 路径不能以反斜杠结尾（否则会转义引号），且不要引号包裹
        batchContent += "/Fo" + objDir + "\\ ";
        batchContent += "\"" + objDir + "\\*.cpp\" ";
        batchContent += "\"" + verilatorIncludePath + "\\verilated.cpp\" ";
        batchContent += "\"" + verilatorIncludePath + "\\verilated_vcd_c.cpp\" ";
        batchContent += "\"" + verilatorIncludePath + "\\verilated_threads.cpp\" ";
        batchContent += "\"" + verilatorIncludePath + "\\verilated_timing.cpp\" ";
        batchContent += "\"" + stubPath + "\" ";
        batchContent += "/I\"" + verilatorIncludePath + "\" ";
        batchContent += "/I\"" + verilatorIncludePath + "\\vltstd\" ";
        batchContent += "/I\"" + objDir + "\" ";
        batchContent += "/link /DLL /MACHINE:X64 ws2_32.lib\n";
        batchContent += "exit /b %errorLevel%\n";
        
        batchFile.Write(batchContent);
        batchFile.Close();
        
        OutputDebugStringA("Batch file created at: ");
        OutputDebugStringA(batchPath.ToUTF8());
        OutputDebugStringA("\n");
    }
    
    // 初始化编译状态（加锁保护）
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_isCompiling = true;
        m_dllCompileSuccess = false;
        m_lastCompileLog.Clear();
    }
    
    // 创建 ProcessRunner 并设置回调
    m_processRunner = std::make_unique<ProcessRunner>();
    
    // 设置输出回调（实时接收编译输出）
    m_processRunner->SetOutputCallback([this](const wxString& output, bool isError) {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_lastCompileLog += output;
        }
        
        // 如果有外部回调，也通知外部
        if (m_outputCallback) {
            m_outputCallback(output, isError);
        }
        
        // 输出到调试窗口
        OutputDebugStringA(isError ? "[ERR] " : "[OUT] ");
        OutputDebugStringA(output.ToUTF8());
    });
    
    // 设置完成回调
    m_processRunner->SetCompletionCallback([this, dllPath, batchPath](int exitCode) {
        OutputDebugStringA(("Compile process finished with exit code: " + std::to_string(exitCode) + "\n").c_str());

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_isCompiling = false;
            m_dllCompileSuccess = (exitCode == 0 && wxFileExists(dllPath));
        }

        if (exitCode == 0 && wxFileExists(dllPath)) {
            OutputDebugStringA("DLL compiled successfully!\n");
        } else {
            OutputDebugStringA("DLL compilation failed!\n");
        }
        
        // 保留批处理文件用于调试（如果编译失败）
        if (m_dllCompileSuccess) {
            wxRemoveFile(batchPath);
        } else {
            OutputDebugStringA(("Batch file kept for debugging: " + batchPath.ToStdString() + "\n").c_str());
        }
    });
    
    // 使用 ProcessRunner 异步执行批处理
    OutputDebugStringA("Starting async compilation...\n");
    if (!m_processRunner->RunBatchFile(batchPath, projectRoot)) {
        errorMsg = wxT("启动编译进程失败");
        { std::lock_guard<std::mutex> lock(m_mutex); m_isCompiling = false; }
        return false;
    }
    
    // 轮询等待编译完成，定期 yield 让 UI 保持响应（spinner 动画等）
    OutputDebugStringA("Waiting for compilation to complete...\n");
    while (m_processRunner->IsRunning()) {
        if (m_processRunner->WaitForCompletion(200))
            break;
        wxYield();
    }
    
    OutputDebugStringA(("Compile returned: " + std::to_string(m_processRunner->GetExitCode()) + "\n").c_str());

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_dllCompileSuccess) {
            OutputDebugStringA("Compile failed\n");
            errorMsg = wxString::Format(wxT("DLL编译失败 (错误码: %d)"), m_processRunner->GetExitCode());
            if (!m_lastCompileLog.IsEmpty()) {
                errorMsg += wxT("\n\n编译日志:\n") + m_lastCompileLog.Left(2000);
            }
            return false;
        }
    }
    
    // 验证 DLL 是否生成
    if (!wxFileExists(dllPath)) {
        OutputDebugStringA("DLL file not found after compile\n");
        errorMsg = wxT("DLL文件未生成");
        return false;
    }
    
    OutputDebugStringA("DLL compile success\n");
    ReportProgress(90, wxString::Format("DLL生成成功: %s", dllPath));
    return true;
}

bool SimulationEngine::IsCompiled(const wxString& topModule) const
{
    wxString dllPath = GetCacheDirectory(topModule) + "\\" + topModule + ".dll";
    return wxFileExists(dllPath);
}

bool SimulationEngine::CleanCache(const wxString& topModule)
{
    wxString cacheDir = GetCacheDirectory(topModule);
    
    try {
        fs::path p(cacheDir.ToStdString());
        if (fs::exists(p)) {
            fs::remove_all(p);
        }
        return true;
    }
    catch (const std::exception& e) {
        OutputDebugStringA(("CleanCache error: " + std::string(e.what()) + "\n").c_str());
        return false;
    }
}

SimulationRunResult SimulationEngine::RunSimulation(const wxString& outputVcdPath)
{
    SimulationRunResult result;

    OutputDebugStringA("=== RunSimulation Start ===\n");

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

    wxString dllPath = cacheDir + "\\" + topModule + ".dll";
    if (!wxFileExists(dllPath)) {
        result.errorMessage = wxT("没有可用的编译结果，请先编译\n找不到: ") + dllPath;
        return result;
    }

    OutputDebugStringA(("DLL found: " + dllPath.ToStdString() + "\n").c_str());
    ReportProgress(10, wxT("找到编译结果，准备仿真..."));

    // 2. 模糊匹配 Testbench 文件
    wxString testbenchPath = FindTestbenchFile(m_projectRoot);
    if (testbenchPath.IsEmpty()) {
        result.errorMessage = wxT("找不到 Testbench 文件\n")
            wxT("请在项目 src 目录下创建名为 test_bench.v / testbench.v / tb_*.v 等文件");
        return result;
    }
    OutputDebugStringA(("Testbench found: " + testbenchPath.ToStdString() + "\n").c_str());
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
    wxString vcdPath = cacheDir + "\\waveform\\wave.vcd";
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
    OutputDebugStringA("=== RunSimulation Success ===\n");
    return result;
}

wxString SimulationEngine::FindTestbenchFile(const wxString& projectRoot) const
{
    wxString srcDir = projectRoot + "\\src";
    if (!wxDir::Exists(srcDir)) {
        OutputDebugStringA("src directory not found\n");
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
                return srcDir + "\\" + candidate;
            }
        }
    }

    OutputDebugStringA("No testbench file found by fuzzy matching\n");
    return wxEmptyString;
}

bool SimulationEngine::CompileSimRunner(const wxString& topModule, const wxString& testbenchPath,
                                        wxString& errorMsg)
{
    OutputDebugStringA("=== CompileSimRunner Start ===\n");
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

    OutputDebugStringA(("Parsed testbench: module=" + tbInfo.moduleName
        + " top=" + tbInfo.topModuleName + "\n").c_str());

    // 2. 生成时间线
    ReportProgress(40, wxT("生成仿真时间线..."));
    TimelineGenerator tlGen;
    Timeline timeline = tlGen.Generate(tbInfo, topModule);

    OutputDebugStringA(("Timeline: events=" + std::to_string(timeline.events.size())
        + " maxTime=" + std::to_string(timeline.maxSimTime) + "\n").c_str());

    // 3. 生成 sim_main.cpp
    ReportProgress(45, wxT("生成 sim_main.cpp..."));
    wxString simMainPath = cacheDir + "\\sim_main.cpp";
    SimMainGenerator mainGen;
    if (!mainGen.Generate(timeline, simMainPath)) {
        errorMsg = wxT("sim_main.cpp 生成失败: ") + mainGen.GetLastError();
        return false;
    }

    OutputDebugStringA(("sim_main.cpp generated at: " + simMainPath.ToStdString() + "\n").c_str());

    // 4. 创建 waveform 目录
    wxString waveDir = cacheDir + "\\waveform";
    CreateDirectoryRecursive(waveDir);

    // 5. 编译 sim_runner.exe
    ReportProgress(50, wxT("编译 sim_runner.exe..."));
    wxString vcvarsPath = FindVCVarsPath();
    if (vcvarsPath.IsEmpty()) {
        errorMsg = wxT("找不到 Visual Studio，请安装 VS 2022 或更高版本");
        return false;
    }

    wxString exePath = cacheDir + "\\sim_runner.exe";
    wxString batchPath = cacheDir + "\\compile_sim.bat";
    wxString verilatorIncludePath = FindVerilatorIncludePath();
    if (verilatorIncludePath.IsEmpty()) {
        errorMsg = wxT("无法定位 Verilator 运行时目录；请设置 VERILATOR_ROOT 或 VERILATOR_BIN。");
        return false;
    }

    // 确保项目根目录是绝对路径
    wxFileName projectRootFn(m_projectRoot);
    projectRootFn.MakeAbsolute();
    wxString projectRoot = projectRootFn.GetFullPath();

    {
        wxFile batchFile(batchPath, wxFile::write);
        if (!batchFile.IsOpened()) {
            errorMsg = wxT("无法创建编译脚本");
            return false;
        }

        wxString batchContent;
        batchContent += "@echo off\n";
        batchContent += "chcp 65001 >nul\n";
        batchContent += "call \"" + vcvarsPath + "\"\n";
        batchContent += "if %errorLevel% neq 0 exit /b %errorLevel%\n";
        batchContent += "cd /d \"" + objDir + "\"\n";               // 切到 objDir，避免空格路径问题
        batchContent += "cl /O2 /MD /EHsc /W3 /std:c++20 ";
        batchContent += "/Fe\"" + exePath + "\" ";
        batchContent += "\"" + simMainPath + "\" ";
        batchContent += "\"" + objDir + "\\*.cpp\" ";
        batchContent += "\"" + verilatorIncludePath + "\\verilated.cpp\" ";
        batchContent += "\"" + verilatorIncludePath + "\\verilated_vcd_c.cpp\" ";
        batchContent += "\"" + verilatorIncludePath + "\\verilated_threads.cpp\" ";
        batchContent += "\"" + verilatorIncludePath + "\\verilated_timing.cpp\" ";

        // sc_time_stub
        wxString stubPath = GetSoftwareDirectory() + "\\main\\Simulation\\sc_time_stub.cpp";
        if (!wxFileExists(stubPath)) {
            stubPath = cacheDir + "\\sc_time_stub.cpp";
            CreateScTimeStub(stubPath);
        }
        batchContent += "\"" + stubPath + "\" ";

        batchContent += "/I\"" + verilatorIncludePath + "\" ";
        batchContent += "/I\"" + verilatorIncludePath + "\\vltstd\" ";
        batchContent += "/I\"" + objDir + "\" ";
        batchContent += "/link /MACHINE:X64 ws2_32.lib\n";
        batchContent += "exit /b %errorLevel%\n";

        batchFile.Write(batchContent);
        batchFile.Close();
    }

    // 使用 ProcessRunner 执行编译
    auto runner = std::make_unique<ProcessRunner>();
    wxString simCompileLog;
    bool simCompileSuccess = false;

    runner->SetOutputCallback([&simCompileLog, this](const wxString& output, bool isError) {
        simCompileLog += output;
        if (m_outputCallback) {
            m_outputCallback(output, isError);
        }
        OutputDebugStringA(isError ? "[SIM-ERR] " : "[SIM-OUT] ");
        OutputDebugStringA(output.ToUTF8());
    });

    runner->SetCompletionCallback([&simCompileSuccess, &exePath](int exitCode) {
        simCompileSuccess = (exitCode == 0 && wxFileExists(exePath));
    });

    if (!runner->RunBatchFile(batchPath, projectRoot)) {
        errorMsg = wxT("启动仿真编译进程失败");
        return false;
    }

    while (runner->IsRunning()) {
        if (runner->WaitForCompletion(200))
            break;
        wxYield();
    }

    if (!simCompileSuccess) {
        errorMsg = wxString::Format(wxT("sim_runner.exe 编译失败 (错误码: %d)"), runner->GetExitCode());
        if (!simCompileLog.IsEmpty()) {
            errorMsg += wxT("\n\n编译日志:\n") + simCompileLog.Left(2000);
        }
        return false;
    }

    // 清理批处理文件
    wxRemoveFile(batchPath);

    OutputDebugStringA("=== CompileSimRunner Success ===\n");
    return true;
}

bool SimulationEngine::ExecuteSimRunner(const wxString& topModule, wxString& errorMsg)
{
    OutputDebugStringA("=== ExecuteSimRunner Start ===\n");
    ReportProgress(80, wxT("运行仿真..."));

    wxString cacheDir = GetCacheDirectory(topModule);
    wxFileName cacheDirFn(cacheDir);
    cacheDirFn.MakeAbsolute();
    cacheDir = cacheDirFn.GetFullPath();

    wxString exePath = cacheDir + "\\sim_runner.exe";
    if (!wxFileExists(exePath)) {
        errorMsg = wxT("找不到 sim_runner.exe");
        return false;
    }

    // 确保 waveform 目录存在
    wxString waveDir = cacheDir + "\\waveform";
    CreateDirectoryRecursive(waveDir);

    // 运行 sim_runner.exe（工作目录设为 cacheDir，因为 VCD 路径是相对的）
    auto runner = std::make_unique<ProcessRunner>();
    wxString runLog;
    bool runSuccess = false;

    runner->SetOutputCallback([&runLog, this](const wxString& output, bool isError) {
        runLog += output;
        if (m_outputCallback) {
            m_outputCallback(output, isError);
        }
    });

    runner->SetCompletionCallback([&runSuccess](int exitCode) {
        runSuccess = (exitCode == 0);
    });

    wxString cmd = "\"" + exePath + "\"";
    if (!runner->RunAsync(cmd, cacheDir)) {
        errorMsg = wxT("启动 sim_runner.exe 失败");
        return false;
    }

    {
        int elapsed = 0;
        while (runner->IsRunning()) {
            if (runner->WaitForCompletion(200))
                break;
            wxYield();
            elapsed += 200;
            if (elapsed >= 60000) {
                runner->Terminate();
                errorMsg = wxT("仿真运行超时（60秒）");
                return false;
            }
        }
    }

    if (!runSuccess) {
        errorMsg = wxString::Format(wxT("仿真运行失败 (错误码: %d)"), runner->GetExitCode());
        if (!runLog.IsEmpty()) {
            errorMsg += wxT("\n\n运行日志:\n") + runLog.Left(2000);
        }
        return false;
    }

    OutputDebugStringA("=== ExecuteSimRunner Success ===\n");
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
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_processRunner && m_isCompiling) {
        OutputDebugStringA("Cancelling compilation...\n");
        m_processRunner->Terminate();
        m_isCompiling = false;
    }
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
    OutputDebugStringA("Executable path: ");
    OutputDebugStringA(path.ToUTF8());
    OutputDebugStringA("\n");
    
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
    OutputDebugStringA("Software directory: ");
    OutputDebugStringA(result.ToUTF8());
    OutputDebugStringA("\n");
    
    return result;
}
