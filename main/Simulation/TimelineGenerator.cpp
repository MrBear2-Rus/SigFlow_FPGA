#include "TimelineGenerator.h"
#include <algorithm>

TimelineGenerator::TimelineGenerator() {}
TimelineGenerator::~TimelineGenerator() {}

Timeline TimelineGenerator::Generate(const TestbenchInfo& tbInfo, const wxString& topModule)
{
    Timeline timeline;
    timeline.topModuleName = topModule.ToStdString();

    // 提取时钟信息
    for (const auto& block : tbInfo.initialBlocks) {
        if (block.hasClock) {
            timeline.clock.signalName = MapSignalToPort(block.clock.signalName, tbInfo);
            timeline.clock.halfPeriod = block.clock.halfPeriod;
            timeline.clock.initialValue = block.clock.initialValue;
            timeline.clock.valid = true;
            break;
        }
    }

    // 收集所有输入信号（从端口映射中推导）
    for (const auto& mapping : tbInfo.portMappings) {
        bool isReg = false;
        for (const auto& sig : tbInfo.regSignals) {
            if (sig == mapping.signalName) { isReg = true; break; }
        }
        if (isReg) {
            timeline.inputSignals.push_back(mapping.portName);
        }
    }

    // 展开所有非时钟 initial 块为绝对时间事件
    for (const auto& block : tbInfo.initialBlocks) {
        if (block.hasClock) {
            // 时钟块的初始赋值也要记录
            for (const auto& evt : block.events) {
                if (evt.type == StimulusEventType::SignalAssign) {
                    TimelineEvent te;
                    te.time = 0;
                    te.signalName = MapSignalToPort(evt.signalName, tbInfo);
                    te.value = evt.value;
                    te.comment = "clock init";
                    timeline.events.push_back(te);
                    timeline.signalInitValues[te.signalName] = te.value;
                }
            }
            continue;
        }

        uint64_t currentTime = 0;
        FlattenInitialBlock(block, tbInfo, timeline.events, currentTime);
        if (currentTime > timeline.maxSimTime) {
            timeline.maxSimTime = currentTime;
        }
    }

    // 使用解析估算的时间，如果更大
    if (tbInfo.maxSimTime > timeline.maxSimTime) {
        timeline.maxSimTime = tbInfo.maxSimTime;
    }

    // 保底：至少运行 200 个时间单位
    if (timeline.maxSimTime < 200) {
        timeline.maxSimTime = 200;
    }

    // 按时间排序
    std::stable_sort(timeline.events.begin(), timeline.events.end(),
        [](const TimelineEvent& a, const TimelineEvent& b) {
            return a.time < b.time;
        });

    // 记录初始值
    for (const auto& evt : timeline.events) {
        if (evt.time == 0 && timeline.signalInitValues.find(evt.signalName) == timeline.signalInitValues.end()) {
            timeline.signalInitValues[evt.signalName] = evt.value;
        }
    }

    return timeline;
}

void TimelineGenerator::FlattenInitialBlock(const InitialBlock& block, const TestbenchInfo& tbInfo,
                                            std::vector<TimelineEvent>& events, uint64_t& currentTime)
{
    for (const auto& evt : block.events) {
        switch (evt.type) {
        case StimulusEventType::Delay:
            currentTime += evt.delayValue;
            break;

        case StimulusEventType::SignalAssign: {
            TimelineEvent te;
            te.time = currentTime;
            te.signalName = MapSignalToPort(evt.signalName, tbInfo);
            te.value = evt.value;
            te.comment = evt.rawText;
            events.push_back(te);
            break;
        }

        case StimulusEventType::PosedgeWait:
        case StimulusEventType::NegedgeWait: {
            // 边沿等待：找时钟的半周期来推算等待时间
            for (const auto& b : tbInfo.initialBlocks) {
                if (b.hasClock) {
                    // 等一个完整周期（上升+下降）
                    currentTime += b.clock.halfPeriod * 2;
                    break;
                }
            }

            TimelineEvent te;
            te.time = currentTime;
            te.signalName = "";
            te.comment = (evt.type == StimulusEventType::PosedgeWait ?
                "@(posedge " : "@(negedge ") + evt.edgeSignal + ") - simplified";
            events.push_back(te);
            break;
        }

        case StimulusEventType::Finish:
        case StimulusEventType::Display:
            break;

        default:
            break;
        }
    }
}

std::string TimelineGenerator::MapSignalToPort(const std::string& tbSignal, const TestbenchInfo& tbInfo)
{
    for (const auto& mapping : tbInfo.portMappings) {
        if (mapping.signalName == tbSignal) {
            return mapping.portName;
        }
    }
    return tbSignal; // 没找到映射则原样返回
}
