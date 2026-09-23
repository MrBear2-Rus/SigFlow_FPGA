#include "SimTimeline.h"

#include <algorithm>

namespace eda {
namespace sim {

TimelineGenerator::TimelineGenerator() = default;
TimelineGenerator::~TimelineGenerator() = default;

Timeline TimelineGenerator::Generate(const TestbenchInfo& tbInfo, const std::string& topModule) {
    Timeline timeline;
    timeline.topModuleName = topModule;

    for (const auto& block : tbInfo.initialBlocks) {
        if (block.hasClock) {
            timeline.clock.signalName = MapSignalToPort(block.clock.signalName, tbInfo);
            timeline.clock.halfPeriod = block.clock.halfPeriod;
            timeline.clock.initialValue = block.clock.initialValue;
            timeline.clock.valid = true;
            break;
        }
    }

    for (const auto& mapping : tbInfo.portMappings) {
        bool isReg = false;
        for (const auto& signal : tbInfo.regSignals) {
            if (signal == mapping.signalName) { isReg = true; break; }
        }
        if (isReg) timeline.inputSignals.push_back(mapping.portName);
    }

    for (const auto& block : tbInfo.initialBlocks) {
        if (block.hasClock) {
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
        std::uint64_t currentTime = 0;
        FlattenInitialBlock(block, tbInfo, timeline.events, currentTime);
        if (currentTime > timeline.maxSimTime) timeline.maxSimTime = currentTime;
    }

    if (tbInfo.maxSimTime > timeline.maxSimTime) timeline.maxSimTime = tbInfo.maxSimTime;
    if (timeline.maxSimTime < 200) timeline.maxSimTime = 200;

    std::stable_sort(timeline.events.begin(), timeline.events.end(),
                     [](const TimelineEvent& a, const TimelineEvent& b) { return a.time < b.time; });

    for (const auto& evt : timeline.events) {
        if (evt.time == 0 &&
            timeline.signalInitValues.find(evt.signalName) == timeline.signalInitValues.end()) {
            timeline.signalInitValues[evt.signalName] = evt.value;
        }
    }
    return timeline;
}

void TimelineGenerator::FlattenInitialBlock(const InitialBlock& block, const TestbenchInfo& tbInfo,
                                            std::vector<TimelineEvent>& events,
                                            std::uint64_t& currentTime) {
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
                for (const auto& other : tbInfo.initialBlocks) {
                    if (other.hasClock) { currentTime += other.clock.halfPeriod * 2; break; }
                }
                TimelineEvent te;
                te.time = currentTime;
                te.comment = (evt.type == StimulusEventType::PosedgeWait ? "@(posedge "
                                                                         : "@(negedge ") +
                             evt.edgeSignal + ") - simplified";
                events.push_back(te);
                break;
            }
            case StimulusEventType::Finish:
            case StimulusEventType::Display:
            default:
                break;
        }
    }
}

std::string TimelineGenerator::MapSignalToPort(const std::string& tbSignal,
                                               const TestbenchInfo& tbInfo) {
    for (const auto& mapping : tbInfo.portMappings) {
        if (mapping.signalName == tbSignal) return mapping.portName;
    }
    return tbSignal;
}

} // namespace sim
} // namespace eda
