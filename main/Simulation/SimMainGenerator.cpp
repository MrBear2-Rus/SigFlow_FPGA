// P1-4：SimMainGenerator 已移入 InnerPlugin/eda-sim-verilator（wx-free）。
// 本文件保留同签名的 wx 适配层，供 SimulationEngine 使用。
#include "SimMainGenerator.h"

#include "SimMainCodeGen.h"
#include "SimModelBridge.h"

SimMainGenerator::SimMainGenerator() = default;
SimMainGenerator::~SimMainGenerator() = default;

bool SimMainGenerator::Generate(const Timeline& timeline, const wxString& outputPath,
                                const wxString& vcdRelativePath) {
    eda::sim::SimMainGenerator generator;
    const bool ok = generator.Generate(simbridge::ToPlugin(timeline), simbridge::S(outputPath),
                                       simbridge::S(vcdRelativePath));
    m_lastError = simbridge::W(generator.LastError());
    return ok;
}
