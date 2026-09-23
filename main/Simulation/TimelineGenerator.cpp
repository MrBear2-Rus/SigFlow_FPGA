// P1-4：TimelineGenerator 已移入 InnerPlugin/eda-sim-verilator（wx-free）。
// 本文件保留同签名的 wx 适配层，供 SimulationEngine 使用。
#include "TimelineGenerator.h"

#include "SimModelBridge.h"
#include "SimTimeline.h"

TimelineGenerator::TimelineGenerator() = default;
TimelineGenerator::~TimelineGenerator() = default;

Timeline TimelineGenerator::Generate(const TestbenchInfo& tbInfo, const wxString& topModule) {
    eda::sim::TimelineGenerator generator;
    const eda::sim::Timeline pluginTimeline =
        generator.Generate(simbridge::ToPlugin(tbInfo), simbridge::S(topModule));
    m_lastError = simbridge::W(generator.LastError());
    return simbridge::FromPlugin(pluginTimeline);
}
