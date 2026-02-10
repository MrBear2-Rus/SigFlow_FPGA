#include "VerilatorRunner.h"
#include <wx/process.h>
#include <wx/txtstrm.h>
#include <wx/utils.h>
#include <wx/filefn.h>

VerilatorRunner::VerilatorRunner()
    : m_verilatorPath(FindSystemVerilator())
{
}

VerilatorRunner::~VerilatorRunner()
{
}

void VerilatorRunner::SetVerilatorPath(const wxString& path)
{
    m_verilatorPath = path;
}

void VerilatorRunner::SetWorkingDirectory(const wxString& dir)
{
    m_workingDir = dir;
}

wxString VerilatorRunner::FindSystemVerilator()
{
    // 检查环境变量
    wxString verilatorRoot;
    if (wxGetEnv("VERILATOR_ROOT", &verilatorRoot) && !verilatorRoot.IsEmpty()) {
        wxString path = verilatorRoot + "\\bin\\verilator.exe";
        if (wxFileExists(path)) {
            return path;
        }
    }

    // 检查PATH
    wxString pathEnv;
    if (wxGetEnv("PATH", &pathEnv)) {
        wxArrayString paths = wxSplit(pathEnv, ';');
        for (const auto& p : paths) {
            wxString verilatorPath = p + "\\verilator.exe";
            if (wxFileExists(verilatorPath)) {
                return verilatorPath;
            }
        }
    }

    // 检查常见安装位置
    const wxString commonPaths[] = {
        "C:\\verilator\\bin\\verilator.exe",
        "C:\\Program Files\\verilator\\bin\\verilator.exe",
        "C:\\ProgramData\\chocolatey\\bin\\verilator.exe",
    };

    for (const auto& p : commonPaths) {
        if (wxFileExists(p)) {
            return p;
        }
    }

    // 返回默认值，期望在PATH中
    return "verilator";
}

bool VerilatorRunner::IsVerilatorAvailable() const
{
    if (m_verilatorPath.IsEmpty()) {
        return false;
    }

    // 如果路径是相对路径或只有命令名，检查是否能执行
    if (!wxFileExists(m_verilatorPath) && !m_verilatorPath.Contains("\\") && !m_verilatorPath.Contains("/")) {
        // 尝试在PATH中查找
        wxArrayString output, errors;
        wxExecute(m_verilatorPath + " --version", output, errors, wxEXEC_SYNC | wxEXEC_HIDE_CONSOLE);
        return !output.IsEmpty();
    }

    return wxFileExists(m_verilatorPath);
}

wxString VerilatorRunner::GetVersion()
{
    if (!IsVerilatorAvailable()) {
        return "Verilator不可用";
    }

    wxArrayString output, errors;
    int ret = wxExecute(m_verilatorPath + " --version", output, errors, 
                        wxEXEC_SYNC | wxEXEC_HIDE_CONSOLE);

    if (ret == 0 && !output.IsEmpty()) {
        return output[0];
    }

    return "无法获取版本";
}

bool VerilatorRunner::ValidateConfig(const VerilatorConfig& config, wxString& error)
{
    if (config.topModule.IsEmpty()) {
        error = "顶层模块名不能为空";
        return false;
    }

    if (config.verilogFiles.empty()) {
        error = "至少需要一个Verilog文件";
        return false;
    }

    // 检查所有源文件是否存在
    for (const auto& file : config.verilogFiles) {
        if (!wxFileExists(file)) {
            error = wxString::Format("文件不存在: %s", file);
            return false;
        }
    }

    return true;
}

wxString VerilatorRunner::BuildCommand(const VerilatorConfig& config)
{
    wxString cmd = "\"" + m_verilatorPath + "\"";

    // 基本选项
    cmd += " -cc";                          // 生成C++代码
    
    if (config.enableWall) {
        cmd += " -Wall";                    // 启用所有警告
    }

    if (config.enableTrace) {
        cmd += " --trace";                  // 启用波形跟踪
        cmd += " --trace-underscore";
        cmd += " --trace-structs";
    }

    // 指定顶层模块
    cmd += " --top-module " + config.topModule;

    // 指定输出目录
    wxString mdir = config.outputDir;
    if (mdir.IsEmpty()) {
        mdir = "obj_dir";
    }
    cmd += " --Mdir " + mdir;

    // 生成共享库
    cmd += " --build --shared";

    // 额外选项
    if (!config.additionalFlags.IsEmpty()) {
        cmd += " " + config.additionalFlags;
    }

    // 添加源文件
    for (const auto& file : config.verilogFiles) {
        cmd += " \"" + file + "\"";
    }

    return cmd;
}

int VerilatorRunner::Execute(const wxString& cmd, VerilatorResult& result)
{
    wxLogMessage("执行命令: %s", cmd);

    wxProcess process;
    process.Redirect();

    int flags = wxEXEC_SYNC | wxEXEC_HIDE_CONSOLE;
    if (!m_workingDir.IsEmpty()) {
        wxSetWorkingDirectory(m_workingDir);
    }

    // 使用 wxArrayString 版本，更稳定
    wxArrayString outputArr, errorArr;
    int ret = wxExecute(cmd, outputArr, errorArr, flags);
    
    // 合并输出
    for (const auto& line : outputArr) {
        result.stdoutOutput += line + "\n";
    }
    for (const auto& line : errorArr) {
        result.stderrOutput += line + "\n";
    }

    return ret;
}

VerilatorResult VerilatorRunner::Run(const VerilatorConfig& config)
{
    VerilatorResult result;

    // 验证配置
    wxString error;
    if (!ValidateConfig(config, error)) {
        result.errorMessage = error;
        return result;
    }

    // 检查Verilator是否可用
    if (!IsVerilatorAvailable()) {
        result.errorMessage = wxString::Format("Verilator不可用: %s", m_verilatorPath);
        return result;
    }

    // 构建并执行命令
    wxString cmd = BuildCommand(config);
    result.exitCode = Execute(cmd, result);

    // 确定结果
    result.success = (result.exitCode == 0);
    
    if (!result.success) {
        result.errorMessage = wxString::Format("Verilator执行失败 (退出码: %d)", 
                                               result.exitCode);
        if (!result.stderrOutput.IsEmpty()) {
            result.errorMessage += "\n错误输出:\n" + result.stderrOutput;
        }
    }

    // 设置生成的目录
    result.generatedCppDir = config.outputDir;
    if (result.generatedCppDir.IsEmpty()) {
        result.generatedCppDir = "obj_dir";
    }

    return result;
}
