#pragma once
#include <eda/api/Types.h>
#include <eda/api/port_manifest.hpp>
namespace eda::virtual_board {
// Shared production gate used before code generation AND before runner startup.
// No model factory, Verilator instance or wx object is created here.
Json ValidateVirtualProject(const Json& sessionConfig,const PortManifest&,bool synthetic=false);
}
