#pragma once

#include <wx/wx.h>
#include <string>
#include <vector>
#include <map>
#include "StimulusParser.h"

// 时间线上的单个事件（绝对时间）
struct TimelineEvent {
    uint64_t time;              // 绝对仿真时间
    std::string signalName;     // 信号名
    std::string value;          // 赋值的值
    std::string comment;        // 注释（调试用）
};

// 时钟信息（用于生成时钟驱动代码）
struct TimelineClock {
    std::string signalName;
    uint64_t halfPeriod;
    std::string initialValue;
    bool valid = false;
};

// 完整的时间线
struct Timeline {
    std::string topModuleName;                  // 顶层模块名（Verilator 生成的类名前缀）
    std::vector<TimelineEvent> events;          // 按时间排序的事件序列
    TimelineClock clock;                        // 时钟信息
    uint64_t maxSimTime = 0;                    // 总仿真时间
    std::vector<std::string> inputSignals;      // 所有输入信号名
    std::map<std::string, std::string> signalInitValues; // 信号初始值
};

class TimelineGenerator
{
public:
    TimelineGenerator();
    ~TimelineGenerator();

    // 从 Testbench 解析结果生成时间线
    Timeline Generate(const TestbenchInfo& tbInfo, const wxString& topModule);

    wxString GetLastError() const { return m_lastError; }

private:
    wxString m_lastError;

    // 将 initial 块的事件展开为绝对时间事件
    void FlattenInitialBlock(const InitialBlock& block, const TestbenchInfo& tbInfo,
                             std::vector<TimelineEvent>& events, uint64_t& currentTime);

    // 根据端口映射，将 Testbench 信号名映射为顶层模块端口名
    std::string MapSignalToPort(const std::string& tbSignal, const TestbenchInfo& tbInfo);
};
