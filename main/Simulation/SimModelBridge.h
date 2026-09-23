#pragma once

#include "StimulusParser.h"      // main 模型（std::string 字段）
#include "TimelineGenerator.h"

#include "SimMainCodeGen.h"      // 插件 wx-free 类型
#include "SimTimeline.h"
#include "StimulusModel.h"

#include "platform/PlatformPaths.h"

// main 的模型结构体字段已是 std::string，故转换仅需枚举映射；
// wxString ↔ std::string 仅用于适配层的方法参数 / m_lastError。
namespace simbridge {

inline std::string S(const wxString& value) { return sigflow::platform::Utf8String(value); }
inline wxString W(const std::string& value) { return wxString::FromUTF8(value.c_str()); }

inline eda::sim::TestbenchInfo ToPlugin(const TestbenchInfo& info) {
    eda::sim::TestbenchInfo target;
    target.moduleName = info.moduleName;
    target.topInstanceName = info.topInstanceName;
    target.topModuleName = info.topModuleName;
    for (const auto& mapping : info.portMappings) {
        target.portMappings.push_back({mapping.portName, mapping.signalName});
    }
    target.regSignals = info.regSignals;
    target.wireSignals = info.wireSignals;
    for (const auto& block : info.initialBlocks) {
        eda::sim::InitialBlock converted;
        converted.hasClock = block.hasClock;
        converted.clock.signalName = block.clock.signalName;
        converted.clock.halfPeriod = block.clock.halfPeriod;
        converted.clock.initialValue = block.clock.initialValue;
        for (const auto& evt : block.events) {
            eda::sim::StimulusEvent convertedEvent;
            convertedEvent.type = static_cast<eda::sim::StimulusEventType>(evt.type);
            convertedEvent.signalName = evt.signalName;
            convertedEvent.value = evt.value;
            convertedEvent.delayValue = evt.delayValue;
            convertedEvent.edgeSignal = evt.edgeSignal;
            convertedEvent.rawText = evt.rawText;
            converted.events.push_back(convertedEvent);
        }
        target.initialBlocks.push_back(converted);
    }
    target.maxSimTime = info.maxSimTime;
    return target;
}

inline TestbenchInfo FromPlugin(const eda::sim::TestbenchInfo& info) {
    TestbenchInfo target;
    target.moduleName = info.moduleName;
    target.topInstanceName = info.topInstanceName;
    target.topModuleName = info.topModuleName;
    for (const auto& mapping : info.portMappings) {
        target.portMappings.push_back({mapping.portName, mapping.signalName});
    }
    target.regSignals = info.regSignals;
    target.wireSignals = info.wireSignals;
    for (const auto& block : info.initialBlocks) {
        InitialBlock converted;
        converted.hasClock = block.hasClock;
        converted.clock.signalName = block.clock.signalName;
        converted.clock.halfPeriod = block.clock.halfPeriod;
        converted.clock.initialValue = block.clock.initialValue;
        for (const auto& evt : block.events) {
            StimulusEvent convertedEvent;
            convertedEvent.type = static_cast<StimulusEventType>(evt.type);
            convertedEvent.signalName = evt.signalName;
            convertedEvent.value = evt.value;
            convertedEvent.delayValue = evt.delayValue;
            convertedEvent.edgeSignal = evt.edgeSignal;
            convertedEvent.rawText = evt.rawText;
            converted.events.push_back(convertedEvent);
        }
        target.initialBlocks.push_back(converted);
    }
    target.maxSimTime = info.maxSimTime;
    return target;
}

inline eda::sim::Timeline ToPlugin(const Timeline& timeline) {
    eda::sim::Timeline target;
    target.topModuleName = timeline.topModuleName;
    target.clock.signalName = timeline.clock.signalName;
    target.clock.halfPeriod = timeline.clock.halfPeriod;
    target.clock.initialValue = timeline.clock.initialValue;
    target.clock.valid = timeline.clock.valid;
    target.maxSimTime = timeline.maxSimTime;
    for (const auto& evt : timeline.events) {
        target.events.push_back({evt.time, evt.signalName, evt.value, evt.comment});
    }
    target.inputSignals = timeline.inputSignals;
    target.signalInitValues = timeline.signalInitValues;
    return target;
}

inline Timeline FromPlugin(const eda::sim::Timeline& timeline) {
    Timeline target;
    target.topModuleName = timeline.topModuleName;
    target.clock.signalName = timeline.clock.signalName;
    target.clock.halfPeriod = timeline.clock.halfPeriod;
    target.clock.initialValue = timeline.clock.initialValue;
    target.clock.valid = timeline.clock.valid;
    target.maxSimTime = timeline.maxSimTime;
    for (const auto& evt : timeline.events) {
        TimelineEvent converted;
        converted.time = evt.time;
        converted.signalName = evt.signalName;
        converted.value = evt.value;
        converted.comment = evt.comment;
        target.events.push_back(converted);
    }
    target.inputSignals = timeline.inputSignals;
    target.signalInitValues = timeline.signalInitValues;
    return target;
}

} // namespace simbridge
