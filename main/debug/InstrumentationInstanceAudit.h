#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace sigflow::debug {

struct InstrumentationInstanceCount {
    std::string type;
    std::uint64_t count = 0;
};

struct InstrumentationInstanceInventory {
    std::string schemaVersion = "1.0";
    std::string sourceFingerprint;
    std::string topModule;
    bool complete = false;
    std::vector<InstrumentationInstanceCount> instances;
};

struct InstrumentationInstanceAudit {
    bool comparable = false;
    bool noNewInstances = false;
    std::vector<std::string> addedInstances;
    std::vector<std::string> removedInstances;
    std::vector<std::string> diagnostics;
};

bool LoadInstrumentationInstanceInventoryFromJson(
    const std::string& path,
    const std::string& sourceFingerprint,
    const std::string& topModule,
    InstrumentationInstanceInventory& out,
    std::string& error);

bool CompareInstrumentationInstanceInventories(
    const InstrumentationInstanceInventory& baseline,
    const InstrumentationInstanceInventory& instrumented,
    InstrumentationInstanceAudit& out,
    std::string& error);

} // namespace sigflow::debug
