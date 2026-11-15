#pragma once
#include "port_manifest.hpp"
#include <map>
#include <set>
namespace eda::virtual_board {
struct Provenance {
    std::string id, document_version, url, sha256;
    std::vector<int> physical_pages;
};
struct DevicePad {
    std::string vendor_bank_id;
    bool reviewed = false, available = true;
    std::set<std::string> allowed_io_types;
};
struct DevicePadMap {
    std::string device, package, vendor_package, board_revision;
    std::map<int, DevicePad> pads;
};
struct BoardPowerProfile { std::map<std::string, std::optional<int>> vccio_mv; };
struct IOStandardProfile {
    std::set<Direction> directions;
    std::set<int> vccio_allowed_mv, drive_allowed_ma;
    std::set<std::string> pull_modes;
};
struct ExternalEndpointProfile {
    std::string role; // source -> FPGA input, sink -> FPGA output
    int voltage_mv = 0;
    std::set<std::string> pull_modes;
};
struct ElectricalProfile {
    std::string schema = "sigflow.electrical.v1";
    bool synthetic = false;
    DevicePadMap device;
    BoardPowerProfile power;
    std::map<std::string, IOStandardProfile> standards;
    std::map<int, ExternalEndpointProfile> endpoints;
    std::vector<Provenance> provenance;
};
struct ElectricalIdentity { std::string device, package, board_revision; };
}
