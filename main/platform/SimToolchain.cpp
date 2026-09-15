#include "SimToolchain.h"
#include "PlatformPaths.h"
#include "Log.h"

#include <wx/file.h>
#include <wx/filefn.h>
#include <wx/filename.h>
#include <wx/string.h>

#include <algorithm>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace sigflow::platform {

namespace {

wxString FromUtf8(const std::string& value)
{
    return wxString::FromUTF8(value.c_str());
}

} // namespace

#if defined(_WIN32)

namespace {

// 按版本从新到旧搜索，支持 VS2026/2025/2022/2019（原 SimulationEngine 实现原样迁移）。
wxString FindVCVarsPath()
{
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
                    SIGFLOW_LOG(("Found vcvars: " + path.ToStdString() + "\n").c_str());
                    return path;
                }
            }
        }
    }

    // 最后尝试 vswhere.exe 自动定位（VS 2017+ 附带）
    wxString vswhere = "C:\\Program Files (x86)\\Microsoft Visual Studio\\Installer\\vswhere.exe";
    if (wxFileExists(vswhere)) {
        PlatformProcessRequest request;
        request.executable = vswhere;
        request.arguments = { "-latest", "-property", "installationPath" };
        request.timeoutSeconds = 30;
        const PlatformProcessResult result = PlatformProcess::Run(request);
        if (result.started && result.exitCode == 0 && !result.output.IsEmpty()) {
            wxString installPath = result.output.BeforeFirst('\n');
            installPath.Trim(true).Trim(false);
            wxString vcvars = installPath + "\\VC\\Auxiliary\\Build\\vcvars64.bat";
            if (wxFileExists(vcvars)) {
                return vcvars;
            }
        }
    }

    return wxEmptyString;
}

// 与引擎 RunTool（rawCommandLine=false）一致：argv 模式，由 PlatformProcess 规范转义。
PlatformProcessResult RunCompilerCmd(const wxString& batchPath,
                                     const std::string& workingDirectory,
                                     const PlatformOutputCallback& sink,
                                     const std::function<void(void*)>& onStarted,
                                     const std::function<void()>& onFinished)
{
    PlatformProcessRequest request;
    request.executable = "cmd.exe";
    request.arguments = { "/d", "/s", "/c", batchPath };
    request.workingDirectory = workingDirectory.empty() ? wxString() : FromUtf8(workingDirectory);
    request.timeoutSeconds = 1800;
    request.maxOutputBytes = 16ull * 1024ull * 1024ull;
    request.quoteArguments = true;
    request.onStarted = onStarted;
    request.onFinished = onFinished;
    return PlatformProcess::Run(request, sink);
}

} // namespace

bool SimToolchain::CompileSharedLibrary(const SharedLibRequest& req,
                                        const PlatformOutputCallback& sink,
                                        std::string& error)
{
    error.clear();
    const wxString cacheDir = FromUtf8(req.cacheDir);
    const wxString objDir = FromUtf8(req.objDir);
    const wxString outputPath = FromUtf8(req.outputPath);
    const wxString stubPath = FromUtf8(req.stubSourcePath);
    const wxString verilatorIncludeDir = FromUtf8(req.verilatorIncludeDir);

    const wxString vcvarsPath = FindVCVarsPath();
    if (vcvarsPath.IsEmpty()) {
        error = "找不到 Visual Studio，请安装 VS 2022 或更高版本";
        return false;
    }
    if (verilatorIncludeDir.IsEmpty()) {
        error = "无法定位 Verilator 运行时目录；请设置 VERILATOR_ROOT 或 VERILATOR_BIN。";
        return false;
    }

    const wxString batchPath = JoinPath(cacheDir, "compile_dll.bat");
    {
        wxFile batchFile(batchPath, wxFile::write);
        if (!batchFile.IsOpened()) {
            error = "无法创建编译脚本";
            return false;
        }

        wxString batchContent;
        batchContent += "@echo off\n";
        batchContent += "chcp 65001 >nul\n";
        batchContent += "call \"" + vcvarsPath + "\"\n";
        batchContent += "if %errorLevel% neq 0 exit /b %errorLevel%\n";
        batchContent += "cd /d \"" + objDir + "\"\n";               // 切到 objDir，避免空格路径问题
        batchContent += "cl /LD /O2 /MD /EHsc /W3 /std:c++20 ";     // C++20 标准协程
        batchContent += "/Fe\"" + outputPath + "\" ";
        // /Fo 路径不能以反斜杠结尾（否则会转义引号），且不要引号包裹
        batchContent += "/Fo" + objDir + "\\ ";
        batchContent += "\"" + objDir + "\\*.cpp\" ";
        batchContent += "\"" + verilatorIncludeDir + "\\verilated.cpp\" ";
        batchContent += "\"" + verilatorIncludeDir + "\\verilated_vcd_c.cpp\" ";
        batchContent += "\"" + verilatorIncludeDir + "\\verilated_threads.cpp\" ";
        batchContent += "\"" + verilatorIncludeDir + "\\verilated_timing.cpp\" ";
        for (const std::string& source : req.extraSources) {
            batchContent += "\"" + FromUtf8(source) + "\" ";
        }
        batchContent += "\"" + stubPath + "\" ";
        batchContent += "/I\"" + verilatorIncludeDir + "\" ";
        batchContent += "/I\"" + verilatorIncludeDir + "\\vltstd\" ";
        batchContent += "/I\"" + objDir + "\" ";
        batchContent += "/link /DLL /MACHINE:X64 ws2_32.lib\n";
        batchContent += "exit /b %errorLevel%\n";

        batchFile.Write(batchContent);
        batchFile.Close();

        SIGFLOW_LOG("Batch file created at: ");
        SIGFLOW_LOG(batchPath.ToUTF8().data());
        SIGFLOW_LOG("\n");
    }

    const PlatformProcessResult compileResult =
        RunCompilerCmd(batchPath, req.workingDirectory, sink, req.onStarted, req.onFinished);

    const bool success = compileResult.exitCode == 0 && wxFileExists(outputPath);
    if (success) {
        wxRemoveFile(batchPath);
        return true;
    }

    SIGFLOW_LOG(("Batch file kept for debugging: " + batchPath.ToStdString() + "\n").c_str());
    if (compileResult.timedOut) {
        error = "DLL编译超时（30分钟上限）";
    } else if (!compileResult.started) {
        error = "启动编译进程失败: " + compileResult.errorMessage.ToStdString();
    } else {
        error = "DLL编译失败 (错误码: " + std::to_string(compileResult.exitCode) + ")";
    }
    return false;
}

bool SimToolchain::CompileExecutable(const ExecutableRequest& req,
                                     const PlatformOutputCallback& sink,
                                     std::string& error)
{
    error.clear();
    const wxString cacheDir = FromUtf8(req.cacheDir);
    const wxString objDir = FromUtf8(req.objDir);
    const wxString outputPath = FromUtf8(req.outputPath);
    const wxString mainSourcePath = FromUtf8(req.mainSourcePath);
    const wxString stubPath = FromUtf8(req.stubSourcePath);
    const wxString verilatorIncludeDir = FromUtf8(req.verilatorIncludeDir);

    const wxString vcvarsPath = FindVCVarsPath();
    if (vcvarsPath.IsEmpty()) {
        error = "找不到 Visual Studio，请安装 VS 2022 或更高版本";
        return false;
    }
    if (verilatorIncludeDir.IsEmpty()) {
        error = "无法定位 Verilator 运行时目录；请设置 VERILATOR_ROOT 或 VERILATOR_BIN。";
        return false;
    }

    const wxString batchPath = JoinPath(cacheDir, "compile_sim.bat");
    {
        wxFile batchFile(batchPath, wxFile::write);
        if (!batchFile.IsOpened()) {
            error = "无法创建编译脚本";
            return false;
        }

        wxString batchContent;
        batchContent += "@echo off\n";
        batchContent += "chcp 65001 >nul\n";
        batchContent += "call \"" + vcvarsPath + "\"\n";
        batchContent += "if %errorLevel% neq 0 exit /b %errorLevel%\n";
        batchContent += "cd /d \"" + objDir + "\"\n";               // 切到 objDir，避免空格路径问题
        batchContent += "cl /O2 /MD /EHsc /W3 /std:c++20 ";
        batchContent += "/Fe\"" + outputPath + "\" ";
        batchContent += "\"" + mainSourcePath + "\" ";
        batchContent += "\"" + objDir + "\\*.cpp\" ";
        batchContent += "\"" + verilatorIncludeDir + "\\verilated.cpp\" ";
        batchContent += "\"" + verilatorIncludeDir + "\\verilated_vcd_c.cpp\" ";
        batchContent += "\"" + verilatorIncludeDir + "\\verilated_threads.cpp\" ";
        batchContent += "\"" + verilatorIncludeDir + "\\verilated_timing.cpp\" ";
        batchContent += "\"" + stubPath + "\" ";
        batchContent += "/I\"" + verilatorIncludeDir + "\" ";
        batchContent += "/I\"" + verilatorIncludeDir + "\\vltstd\" ";
        batchContent += "/I\"" + objDir + "\" ";
        batchContent += "/link /MACHINE:X64 ws2_32.lib\n";
        batchContent += "exit /b %errorLevel%\n";

        batchFile.Write(batchContent);
        batchFile.Close();
    }

    const PlatformProcessResult compileResult =
        RunCompilerCmd(batchPath, req.workingDirectory, sink, req.onStarted, req.onFinished);

    if (compileResult.timedOut) {
        error = "sim_runner.exe 编译超时（30分钟上限）";
        return false;
    }
    if (!compileResult.started) {
        error = "启动仿真编译进程失败: " + compileResult.errorMessage.ToStdString();
        return false;
    }
    if (compileResult.exitCode != 0 || !wxFileExists(outputPath)) {
        error = "sim_runner.exe 编译失败 (错误码: " + std::to_string(compileResult.exitCode) + ")";
        return false;
    }

    wxRemoveFile(batchPath);
    return true;
}

#else // POSIX：直接驱动 g++（无批处理、无 shell 字符串，argv 模式）

namespace {

std::vector<std::string> CollectObjectSources(const std::string& objDir)
{
    std::vector<std::string> sources;
    std::error_code ec;
    const std::filesystem::path directory = std::filesystem::u8path(objDir);
    if (!std::filesystem::is_directory(directory, ec)) return sources;
    for (const auto& entry : std::filesystem::directory_iterator(directory, ec)) {
        if (!entry.is_regular_file(ec)) continue;
        const std::string name = entry.path().filename().string();
        if (name.size() > 4 && name.compare(name.size() - 4, 4, ".cpp") == 0) {
            sources.push_back(entry.path().string());
        }
    }
    std::sort(sources.begin(), sources.end());
    return sources;
}

std::vector<wxString> VerilatorRuntimeSources(const std::string& includeDir)
{
    std::vector<wxString> sources;
    const std::string base = JoinPath(includeDir, "verilated");
    sources.push_back(FromUtf8(base + ".cpp"));
    sources.push_back(FromUtf8(base + "_vcd_c.cpp"));
    sources.push_back(FromUtf8(base + "_threads.cpp"));
    sources.push_back(FromUtf8(base + "_timing.cpp"));
    return sources;
}

PlatformProcessResult RunCompilerArgs(const std::vector<wxString>& arguments,
                                      const std::string& workingDirectory,
                                      const PlatformOutputCallback& sink,
                                      const std::function<void(void*)>& onStarted,
                                      const std::function<void()>& onFinished)
{
    PlatformProcessRequest request;
    request.executable = "g++";
    request.arguments = arguments;
    request.workingDirectory = workingDirectory.empty() ? wxString() : FromUtf8(workingDirectory);
    request.timeoutSeconds = 1800;
    request.maxOutputBytes = 16ull * 1024ull * 1024ull;
    request.quoteArguments = true;
    request.onStarted = onStarted;
    request.onFinished = onFinished;
    return PlatformProcess::Run(request, sink);
}

bool OutputExists(const std::string& path)
{
    std::error_code ec;
    return std::filesystem::exists(std::filesystem::u8path(path), ec);
}

} // namespace

bool SimToolchain::CompileSharedLibrary(const SharedLibRequest& req,
                                        const PlatformOutputCallback& sink,
                                        std::string& error)
{
    error.clear();
    std::vector<wxString> arguments;
    arguments.push_back("-shared");
    arguments.push_back("-fPIC");
    arguments.push_back("-O2");
    arguments.push_back("-std=c++20");
    arguments.push_back("-o");
    arguments.push_back(FromUtf8(req.outputPath));
    for (const std::string& source : CollectObjectSources(req.objDir)) {
        arguments.push_back(FromUtf8(source));
    }
    for (const wxString& source : VerilatorRuntimeSources(req.verilatorIncludeDir)) {
        arguments.push_back(source);
    }
    for (const std::string& source : req.extraSources) {
        arguments.push_back(FromUtf8(source));
    }
    arguments.push_back(FromUtf8(req.stubSourcePath));
    arguments.push_back("-I" + FromUtf8(req.verilatorIncludeDir));
    arguments.push_back("-I" + FromUtf8(JoinPath(req.verilatorIncludeDir, "vltstd")));
    arguments.push_back("-I" + FromUtf8(req.objDir));

    const std::string workingDirectory =
        req.workingDirectory.empty() ? req.cacheDir : req.workingDirectory;
    const PlatformProcessResult result =
        RunCompilerArgs(arguments, workingDirectory, sink, req.onStarted, req.onFinished);

    if (result.timedOut) { error = "DLL编译超时（30分钟上限）"; return false; }
    if (!result.started) {
        error = "启动编译进程失败: " + result.errorMessage.ToStdString();
        return false;
    }
    if (result.exitCode != 0 || !OutputExists(req.outputPath)) {
        error = "DLL编译失败 (错误码: " + std::to_string(result.exitCode) + ")";
        return false;
    }
    return true;
}

bool SimToolchain::CompileExecutable(const ExecutableRequest& req,
                                     const PlatformOutputCallback& sink,
                                     std::string& error)
{
    error.clear();
    std::vector<wxString> arguments;
    arguments.push_back("-O2");
    arguments.push_back("-std=c++20");
    arguments.push_back("-o");
    arguments.push_back(FromUtf8(req.outputPath));
    arguments.push_back(FromUtf8(req.mainSourcePath));
    for (const std::string& source : CollectObjectSources(req.objDir)) {
        arguments.push_back(FromUtf8(source));
    }
    for (const wxString& source : VerilatorRuntimeSources(req.verilatorIncludeDir)) {
        arguments.push_back(source);
    }
    arguments.push_back(FromUtf8(req.stubSourcePath));
    arguments.push_back("-I" + FromUtf8(req.verilatorIncludeDir));
    arguments.push_back("-I" + FromUtf8(JoinPath(req.verilatorIncludeDir, "vltstd")));
    arguments.push_back("-I" + FromUtf8(req.objDir));

    const std::string workingDirectory =
        req.workingDirectory.empty() ? req.cacheDir : req.workingDirectory;
    const PlatformProcessResult result =
        RunCompilerArgs(arguments, workingDirectory, sink, req.onStarted, req.onFinished);

    if (result.timedOut) { error = "sim_runner.exe 编译超时（30分钟上限）"; return false; }
    if (!result.started) {
        error = "启动仿真编译进程失败: " + result.errorMessage.ToStdString();
        return false;
    }
    if (result.exitCode != 0 || !OutputExists(req.outputPath)) {
        error = "sim_runner.exe 编译失败 (错误码: " + std::to_string(result.exitCode) + ")";
        return false;
    }
    return true;
}

#endif

} // namespace sigflow::platform
