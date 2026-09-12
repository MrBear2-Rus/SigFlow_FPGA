#pragma once

#include <wx/wx.h>
#include <string>
#include "TimelineGenerator.h"

class SimMainGenerator
{
public:
    SimMainGenerator();
    ~SimMainGenerator();

    // 根据时间线生成 sim_main.cpp 文件
    // outputPath: sim_main.cpp 的输出路径
    // vcdRelativePath: VCD 文件的相对路径（相对于 sim_runner.exe 运行目录）
    bool Generate(const Timeline& timeline, const wxString& outputPath,
                  const wxString& vcdRelativePath = "waveform/wave.vcd");

    wxString GetLastError() const { return m_lastError; }

private:
    wxString m_lastError;

    // 生成头文件 include 部分
    std::string GenerateIncludes(const Timeline& timeline);

    // 生成初始化代码
    std::string GenerateInitialization(const Timeline& timeline);

    // 生成时间线事件驱动代码
    std::string GenerateEventDriving(const Timeline& timeline);

    // 生成收尾代码
    std::string GenerateCleanup();

    // 辅助：将 Verilog 值转为 C++ 值
    std::string VerilogValueToCpp(const std::string& value);
};
