#pragma once
#include "Phase1Faults.h"
namespace eda::virtual_board {
bool ValidatePhase2Fault(const BoardTopology&,const Json& rule);
Json Phase2TopologyOverlay(const Json& activeRules);
std::vector<PinBindingV2> Phase2Bindings(const PortManifest&,const std::vector<PinBindingV2>&,const BoardTopology&,const Json& rules);
}
