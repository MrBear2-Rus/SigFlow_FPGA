#pragma once
#include <eda/api/electrical_profile.hpp>
#include <filesystem>
namespace eda::virtual_board {
bool LoadElectricalProfile(const std::filesystem::path&, ElectricalProfile&, std::vector<Diagnostic>&,
                           bool allow_synthetic = false);
std::vector<Diagnostic> ValidateElectrical(const ElectricalProfile&, const ElectricalIdentity&,
                                          const PortManifest&, const std::vector<PinBindingV2>&);
}
