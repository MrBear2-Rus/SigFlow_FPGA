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

// Optional instrumentation primitives (for example a snapshot mux) are
// audited separately from the default zero-new-instance check. The policy
// records exactly which cell types are expected when the feature is enabled
// and verifies that they disappear from the disabled netlist.
struct InstrumentationOptionalPrimitivePolicy {
    std::vector<std::string> allowedTypes;
    bool enabled = false;
    bool requireAbsentWhenDisabled = true;
};

struct InstrumentationInstanceAudit {
    bool comparable = false;
    bool noNewInstances = false;
    bool optionalPrimitivePolicySatisfied = true;
    std::vector<std::string> addedInstances;
    std::vector<std::string> removedInstances;
    std::vector<std::string> unexpectedOptionalPrimitives;
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

bool CompareInstrumentationInstanceInventories(
    const InstrumentationInstanceInventory& baseline,
    const InstrumentationInstanceInventory& instrumented,
    const InstrumentationOptionalPrimitivePolicy& policy,
    InstrumentationInstanceAudit& out,
    std::string& error);

} // namespace sigflow::debug
