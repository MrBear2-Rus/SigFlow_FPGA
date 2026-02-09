#include "SimulationEngine.h"
#include "ProcessRunner.h"
#include <wx/process.h>
#include <wx/txtstrm.h>
#include <wx/stdpaths.h>
#include <wx/filename.h>
#include <wx/dir.h>
#include <filesystem>
#include <fstream>
#include <windows.h>  // For Windows API
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
    // 注意：VERILATOR_ROOT 是目录，不是可执行文件，所以不能直接返回
    
    // 检查常见安装路径（包括MSYS2）
    // 注意：MSYS2 的 verilator 是脚本，实际可执行文件是 verilator_bin.exe
    const char* commonPaths[] = {
        // MSYS2 路径（优先检查 verilator_bin.exe）
        "C:\\msys64\\mingw64\\bin\\verilator_bin.exe",
        "C:\\msys64\\usr\\bin\\verilator_bin.exe",
        "C:\\msys64\\mingw64\\bin\\verilator.exe",
        "C:\\msys64\\usr\\bin\\verilator.exe",
        // 标准安装路径
        "C:\\verilator\\bin\\verilator.exe",
        "C:\\Program Files\\verilator\\bin\\verilator.exe",
        "C:\\ProgramData\\chocolatey\\bin\\verilator.exe",
    };

    for (const auto& path : commonPaths) {
        if (wxFileExists(path)) {
            return path;
        }
    }

    // 尝试从PATH中查找（优先找 verilator_bin.exe，因为 MSYS2 的 verilator 是脚本）
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

wxString SimulationEngine::FindVCVarsPath() const
{
    // Visual Studio 2022 常见路径
    const char* vcvarsPaths[] = {
        "C:\\Program Files\\Microsoft Visual Studio\\2022\\Community\\VC\\Auxiliary\\Build\\vcvars64.bat",
        "C:\\Program Files\\Microsoft Visual Studio\\2022\\Professional\\VC\\Auxiliary\\Build\\vcvars64.bat",
        "C:\\Program Files\\Microsoft Visual Studio\\2022\\Enterprise\\VC\\Auxiliary\\Build\\vcvars64.bat",
        "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Auxiliary\\Build\\vcvars64.bat",
    };

    for (const auto& path : vcvarsPaths) {
        if (wxFileExists(path)) {
            return path;
        }
    }

    // Visual Studio 2019 路径
    const char* vcvarsPaths2019[] = {
        "C:\\Program Files (x86)\\Microsoft Visual Studio\\2019\\Community\\VC\\Auxiliary\\Build\\vcvars64.bat",
        "C:\\Program Files (x86)\\Microsoft Visual Studio\\2019\\Professional\\VC\\Auxiliary\\Build\\vcvars64.bat",
        "C:\\Program Files (x86)\\Microsoft Visual Studio\\2019\\Enterprise\\VC\\Auxiliary\\Build\\vcvars64.bat",
        "C:\\Program Files (x86)\\Microsoft Visual Studio\\2019\\BuildTools\\VC\\Auxiliary\\Build\\vcvars64.bat",
    };

    for (const auto& path : vcvarsPaths2019) {
        if (wxFileExists(path)) {
            return path;
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
    wxString cmd = verilatorPath;
    cmd += " -cc";
    cmd += " --Wno-DECLFILENAME";
    cmd += " --Mdir " + objDir;
    cmd += " --top-module " + topModule;
    cmd += " --trace --trace-underscore --trace-structs";
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
    
    const char* stubContent = 
        "// Stub for sc_time_stamp function required by Verilator\n"
        "#include <cstdint>\n"
        "\n"
        "static uint64_t g_sim_time = 0;\n"
        "\n"
        "extern \"C\" double sc_time_stamp() {\n"
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
        errorMsg = wxT("找不到 Visual Studio，请安装 VS 2022");
        return false;
    }
    OutputDebugStringA("Found vcvars: ");
    OutputDebugStringA(vcvarsPath.ToUTF8());
    OutputDebugStringA("\n");
    
    // 创建 sc_time_stub.cpp 如果不存在
    wxString stubPath = projectRoot + "\\Simulation\\sc_time_stub.cpp";
    OutputDebugStringA("Stub path: ");
    OutputDebugStringA(stubPath.ToUTF8());
    OutputDebugStringA("\n");
    
    if (!wxFileExists(stubPath)) {
        OutputDebugStringA("Stub file not found, creating...\n");
        CreateScTimeStub(stubPath);
    } else {
        OutputDebugStringA("Stub file already exists\n");
    }
    
    // 检查 stub 文件是否真的存在
    if (!wxFileExists(stubPath)) {
        OutputDebugStringA("ERROR: Failed to create stub file\n");
        errorMsg = wxT("无法创建 sc_time_stub.cpp");
        return false;
    }
    
    // 创建临时批处理文件来执行编译
    OutputDebugStringA("Creating temp batch file...\n");
    wxString batchPath = cacheDir + "\\compile_dll.bat";
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
        batchContent += "cl /LD /O2 /MD /EHsc /W3 ";
        batchContent += "/Fe\"" + dllPath + "\" ";
        // /Fo 路径不能以反斜杠结尾（否则会转义引号），且不要引号包裹
        batchContent += "/Fo" + objDir + "\\ ";
        batchContent += "\"" + objDir + "\\*.cpp\" ";
        batchContent += "\"C:\\msys64\\mingw64\\share\\verilator\\include\\verilated.cpp\" ";
        batchContent += "\"C:\\msys64\\mingw64\\share\\verilator\\include\\verilated_vcd_c.cpp\" ";
        batchContent += "\"C:\\msys64\\mingw64\\share\\verilator\\include\\verilated_threads.cpp\" ";
        batchContent += "\"" + safeStubPath + "\" ";
        batchContent += "/I\"C:\\msys64\\mingw64\\share\\verilator\\include\" ";
        batchContent += "/I\"C:\\msys64\\mingw64\\share\\verilator\\include\\vltstd\" ";
        batchContent += "/I\"" + objDir + "\" ";
        batchContent += "/link /DLL /MACHINE:X64 ws2_32.lib\n";
        batchContent += "exit /b %errorLevel%\n";
        
        batchFile.Write(batchContent);
        batchFile.Close();
        
        OutputDebugStringA("Batch file created at: ");
        OutputDebugStringA(batchPath.ToUTF8());
        OutputDebugStringA("\n");
    }
    
    // 初始化编译状态
    m_isCompiling = true;
    m_dllCompileSuccess = false;
    m_lastCompileLog.Clear();
    
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
        
        m_isCompiling = false;
        m_dllCompileSuccess = (exitCode == 0 && wxFileExists(dllPath));
        
        if (m_dllCompileSuccess) {
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
        m_isCompiling = false;
        return false;
    }
    
    // 同步等待编译完成（因为 CompileToDll 是同步接口）
    // 如果需要异步，可以修改这里，但当前设计是同步等待
    OutputDebugStringA("Waiting for compilation to complete...\n");
    m_processRunner->WaitForCompletion(INFINITE);
    
    OutputDebugStringA(("Compile returned: " + std::to_string(m_processRunner->GetExitCode()) + "\n").c_str());
    
    if (!m_dllCompileSuccess) {
        OutputDebugStringA("Compile failed\n");
        errorMsg = wxString::Format(wxT("DLL编译失败 (错误码: %d)"), m_processRunner->GetExitCode());
        if (!m_lastCompileLog.IsEmpty()) {
            errorMsg += wxT("\n\n编译日志:\n") + m_lastCompileLog.Left(2000);
        }
        return false;
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
    
    if (!m_lastResult.success || m_lastResult.dllPath.IsEmpty()) {
        result.errorMessage = "没有可用的编译结果，请先编译";
        return result;
    }

    // TODO: 实现DLL加载和仿真运行
    // 这里需要实现DLL的动态加载和波形生成
    
    result.errorMessage = "仿真运行功能尚未实现";
    return result;
}

bool SimulationEngine::IsCompiling() const
{
    return m_isCompiling;
}

void SimulationEngine::CancelCompile()
{
    if (m_processRunner && m_isCompiling) {
        OutputDebugStringA("Cancelling compilation...\n");
        m_processRunner->Terminate();
        m_isCompiling = false;
    }
}
