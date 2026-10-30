#pragma once

#include "agent/gui/AgentViewModel.h"
#include <json/json.h>

namespace agent::gui {

// Project only fields present in the authenticated v1 run response.  The
// caller binds the result to the frozen project/session/run before applying it.
bool ProjectV1Run(const Json::Value& data, vm::RunSnapshot& out, std::string& error);
bool ProjectTeachingRun(const Json::Value& data, vm::RunSnapshot& out, std::string& error);
vm::CapabilitySet ProjectCapabilities(const Json::Value& data);

}  // namespace agent::gui
