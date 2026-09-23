// P1-4：StimulusParser 已移入 InnerPlugin/eda-sim-verilator（wx-free）。
// 本文件保留同签名的 wx 适配层，供 SimulationEngine 使用。
#include "StimulusParser.h"

#include "SimModelBridge.h"
#include "StimulusModel.h"

StimulusParser::StimulusParser() = default;
StimulusParser::~StimulusParser() = default;

bool StimulusParser::Parse(const wxString& filePath, TestbenchInfo& result) {
    eda::sim::StimulusParser parser;
    eda::sim::TestbenchInfo pluginInfo;
    const bool ok = parser.Parse(simbridge::S(filePath), pluginInfo);
    result = simbridge::FromPlugin(pluginInfo);
    m_lastError = simbridge::W(parser.LastError());
    return ok;
}

bool StimulusParser::ParseCode(const std::string& code, TestbenchInfo& result) {
    eda::sim::StimulusParser parser;
    eda::sim::TestbenchInfo pluginInfo;
    const bool ok = parser.ParseCode(code, pluginInfo);
    result = simbridge::FromPlugin(pluginInfo);
    m_lastError = simbridge::W(parser.LastError());
    return ok;
}
