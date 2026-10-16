#include "InstrumentationInstanceAudit.h"

#include <json/json.h>

#include <algorithm>
#include <fstream>
#include <map>

namespace sigflow::debug {
namespace {

bool ReadJson(const std::string& path, Json::Value& root, std::string& error)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        error = "unable to open netlist JSON: " + path;
        return false;
    }
    Json::CharReaderBuilder reader;
    reader["rejectDupKeys"] = true;
    if (!Json::parseFromStream(reader, input, &root, &error) || !root.isObject()) {
        if (error.empty()) error = "netlist JSON root is not an object";
        return false;
    }
    return true;
}

using CountMap = std::map<std::string, std::uint64_t>;

bool BuildCounts(const Json::Value& module, CountMap& counts, std::string& error)
{
    if (!module.isObject() || !module["cells"].isObject()) {
        error = "netlist top module has no cells object";
        return false;
    }
    for (const auto& name : module["cells"].getMemberNames()) {
        const Json::Value& cell = module["cells"][name];
        if (!cell.isObject() || !cell["type"].isString() || cell["type"].asString().empty()) {
            error = "netlist cell has no type: " + name;
            return false;
        }
        ++counts[cell["type"].asString()];
    }
    return true;
}

std::string CountText(const std::string& type, std::uint64_t count)
{
    return type + " x" + std::to_string(count);
}

} // namespace

bool LoadInstrumentationInstanceInventoryFromJson(
    const std::string& path,
    const std::string& sourceFingerprint,
    const std::string& topModule,
    InstrumentationInstanceInventory& out,
    std::string& error)
{
    error.clear();
    out = InstrumentationInstanceInventory();
    if (sourceFingerprint.empty() || topModule.empty()) {
        error = "netlist inventory requires source fingerprint and top module";
        return false;
    }
    Json::Value root;
    if (!ReadJson(path, root, error)) return false;
    if (!root["modules"].isObject() || !root["modules"][topModule].isObject()) {
        error = "netlist top module is missing: " + topModule;
        return false;
    }
    CountMap counts;
    if (!BuildCounts(root["modules"][topModule], counts, error)) return false;
    out.sourceFingerprint = sourceFingerprint;
    out.topModule = topModule;
    out.complete = true;
    for (const auto& [type, count] : counts) out.instances.push_back({type, count});
    return true;
}

bool CompareInstrumentationInstanceInventories(
    const InstrumentationInstanceInventory& baseline,
    const InstrumentationInstanceInventory& instrumented,
    InstrumentationInstanceAudit& out,
    std::string& error)
{
    return CompareInstrumentationInstanceInventories(
        baseline, instrumented, InstrumentationOptionalPrimitivePolicy(), out, error);
}

bool CompareInstrumentationInstanceInventories(
    const InstrumentationInstanceInventory& baseline,
    const InstrumentationInstanceInventory& instrumented,
    const InstrumentationOptionalPrimitivePolicy& policy,
    InstrumentationInstanceAudit& out,
    std::string& error)
{
    error.clear();
    out = InstrumentationInstanceAudit();
    if (baseline.schemaVersion != "1.0" || instrumented.schemaVersion != "1.0") {
        error = "unsupported netlist inventory version";
        return false;
    }
    if (!baseline.complete || !instrumented.complete) {
        out.diagnostics.push_back("baseline or instrumented netlist inventory is incomplete");
    }
    if (baseline.topModule != instrumented.topModule) {
        out.diagnostics.push_back("top module differs");
    }
    if (baseline.sourceFingerprint != instrumented.sourceFingerprint) {
        out.diagnostics.push_back("source fingerprint differs");
    }
    out.comparable = out.diagnostics.empty();
    if (!out.comparable) return true;

    CountMap base;
    CountMap instrumentedCounts;
    for (const auto& item : baseline.instances) base[item.type] += item.count;
    for (const auto& item : instrumented.instances) instrumentedCounts[item.type] += item.count;
    std::map<std::string, std::uint64_t> all = base;
    for (const auto& [type, count] : instrumentedCounts) all[type] += count;
    for (const auto& [type, ignored] : all) {
        const auto oldCount = base.count(type) ? base[type] : 0;
        const auto newCount = instrumentedCounts.count(type) ? instrumentedCounts[type] : 0;
        if (newCount > oldCount) out.addedInstances.push_back(CountText(type, newCount - oldCount));
        if (oldCount > newCount) out.removedInstances.push_back(CountText(type, oldCount - newCount));
    }
    out.noNewInstances = out.addedInstances.empty();

    if (!policy.allowedTypes.empty()) {
        for (const auto& [type, ignored] : all) {
            const auto oldCount = base.count(type) ? base[type] : 0;
            const auto newCount = instrumentedCounts.count(type) ? instrumentedCounts[type] : 0;
            const auto allowed = std::find(policy.allowedTypes.begin(),
                                           policy.allowedTypes.end(), type) !=
                                 policy.allowedTypes.end();
            const bool unexpected = policy.enabled
                ? (!allowed && newCount > oldCount)
                : (policy.requireAbsentWhenDisabled && allowed && newCount > 0);
            if (unexpected) {
                out.unexpectedOptionalPrimitives.push_back(
                    CountText(type, policy.enabled ? (newCount - oldCount) : newCount));
            }
        }
    }
    out.optionalPrimitivePolicySatisfied = out.unexpectedOptionalPrimitives.empty();
    if (!out.optionalPrimitivePolicySatisfied) {
        out.diagnostics.push_back(policy.enabled
            ? "instrumented netlist contains unexpected optional primitive types"
            : "disabled optional primitive policy detected added primitive instances");
    }
    return true;
}

} // namespace sigflow::debug
