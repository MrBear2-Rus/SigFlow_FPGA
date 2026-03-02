#pragma once

#include <wx/wx.h>
#include <string>
#include <vector>
#include <functional>

// Verilator运行配置
struct VerilatorConfig {
    wxString topModule;                     // 顶层模块名
    std::vector<wxString> verilogFiles;     // Verilog源文件列表
    wxString outputDir;                     // 输出目录
    bool enableTrace = true;                // 是否启用波形跟踪
    bool enableWall = true;                 // 是否启用所有警告
    wxString additionalFlags;               // 额外的编译选项
};

// Verilator运行结果
struct VerilatorResult {
    bool success = false;
    int exitCode = -1;
    wxString stdoutOutput;
    wxString stderrOutput;
    wxString errorMessage;
    wxString generatedCppDir;               // 生成的C++代码目录
};

// 简单的Verilator调用封装类
class VerilatorRunner
{
public:
    VerilatorRunner();
    ~VerilatorRunner();

    // 设置Verilator可执行文件路径（如果不设置则自动查找）
    void SetVerilatorPath(const wxString& path);
    
    // 设置工作目录
    void SetWorkingDirectory(const wxString& dir);

    // 运行Verilator生成C++代码
    VerilatorResult Run(const VerilatorConfig& config);

    // 静态辅助函数：查找系统中的Verilator
    static wxString FindSystemVerilator();

    // 检查Verilator是否可用
    bool IsVerilatorAvailable() const;

    // 获取Verilator版本
    wxString GetVersion();

private:
    wxString m_verilatorPath;
    wxString m_workingDir;

    // 构建命令行
    wxString BuildCommand(const VerilatorConfig& config);

    // 执行命令
    int Execute(const wxString& cmd, VerilatorResult& result);

    // 验证配置
    bool ValidateConfig(const VerilatorConfig& config, wxString& error);
};
