#pragma once

#include <wx/wx.h>
#include <string>
#include <vector>
#include <map>
#include <regex>

// 时间线事件类型
enum class StimulusEventType {
    SignalAssign,       // 信号赋值: signal = value
    Delay,              // 延迟: #N
    PosedgeWait,        // 上升沿等待: @(posedge clk)
    NegedgeWait,        // 下降沿等待: @(negedge clk)
    Finish,             // $finish
    Display,            // $display
    ClockDef,           // 时钟定义: forever #N clk = ~clk
};

// 单个激励事件
struct StimulusEvent {
    StimulusEventType type;
    std::string signalName;
    std::string value;          // 赋值的值（字符串形式）
    uint64_t delayValue = 0;    // 延迟值
    std::string edgeSignal;     // 边沿信号名
    std::string rawText;        // 原始文本（调试用）
};

// 时钟定义
struct ClockDef {
    std::string signalName;
    uint64_t halfPeriod = 0;    // 半周期（单位取决于 timescale）
    std::string initialValue;   // 初始值
};

// initial 块
struct InitialBlock {
    std::vector<StimulusEvent> events;
    bool hasClock = false;
    ClockDef clock;
};

// 顶层模块端口（从 Testbench 的实例化中提取）
struct TBPortMapping {
    std::string portName;       // 模块端口名
    std::string signalName;     // Testbench 内部信号名
};

// Testbench 解析结果
struct TestbenchInfo {
    std::string moduleName;                     // Testbench 模块名
    std::string topInstanceName;                // 顶层模块实例名
    std::string topModuleName;                  // 顶层模块定义名
    std::vector<TBPortMapping> portMappings;    // 端口映射
    std::vector<std::string> regSignals;        // reg 信号列表
    std::vector<std::string> wireSignals;       // wire 信号列表
    std::vector<InitialBlock> initialBlocks;    // 所有 initial 块
    uint64_t maxSimTime = 0;                    // 估算的最大仿真时间
};

class StimulusParser
{
public:
    StimulusParser();
    ~StimulusParser();

    // 解析 Testbench 文件，返回解析结果
    bool Parse(const wxString& filePath, TestbenchInfo& result);

    // 解析 Testbench 代码字符串
    bool ParseCode(const std::string& code, TestbenchInfo& result);

    // 获取最后的错误信息
    wxString GetLastError() const { return m_lastError; }

private:
    wxString m_lastError;

    // 预处理：去除注释、合并多行
    std::string Preprocess(const std::string& code);

    // 解析模块声明（提取 reg/wire 信号）
    void ParseSignalDeclarations(const std::string& code, TestbenchInfo& info);

    // 解析顶层模块实例化
    void ParseModuleInstantiation(const std::string& code, TestbenchInfo& info);

    // 解析所有 initial 块
    void ParseInitialBlocks(const std::string& code, TestbenchInfo& info);

    // 解析单个 initial 块内容
    InitialBlock ParseSingleInitialBlock(const std::string& blockContent);

    // 辅助：解析赋值语句
    StimulusEvent ParseAssignment(const std::string& line);

    // 辅助：估算最大仿真时间
    uint64_t EstimateMaxSimTime(const TestbenchInfo& info);
};
