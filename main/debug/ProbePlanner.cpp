#include "ProbePlanner.h"

#include <json/json.h>

#include <algorithm>
#include <map>
#include <set>
#include <sstream>

namespace sigflow {
namespace debug {

namespace {

int KindRank(const std::string& kind)
{
    if (kind == "context") return 0;
    if (kind == "trigger") return 1;
    if (kind == "protocol") return 2;
    if (kind == "symptom") return 3;
    if (kind == "upstream") return 4;
    return 5;
}

Json::Value EntryJson(const ProbePlanEntry& entry)
{
    Json::Value value(Json::objectValue);
    value["signal"] = entry.signal;
    value["reason"] = entry.reason;
    value["kind"] = entry.kind;
    value["clock_domain"] = entry.clockDomain;
    value["group_key"] = entry.groupKey;
    value["width"] = entry.width;
    value["priority"] = entry.priority;
    value["required"] = entry.required;
    value["selected"] = entry.selected;
    value["needs_new_build"] = entry.needsNewBuild;
    value["diagnostic"] = entry.diagnostic;
    return value;
}

Json::Value GroupJson(const ProbePlanGroup& group)
{
    Json::Value value(Json::objectValue);
    value["group_key"] = group.groupKey;
    value["clock_domain"] = group.clockDomain;
    value["kind"] = group.kind;
    value["entry_count"] = static_cast<Json::UInt64>(group.entryCount);
    value["selected_bits"] = static_cast<Json::UInt64>(group.selectedBits);
    return value;
}

} // namespace

std::string ProbePlan::ToJson() const
{
    Json::Value root(Json::objectValue);
    root["schema_version"] = schemaVersion;
    root["capacity_bits"] = static_cast<Json::UInt64>(capacityBits);
    root["used_bits"] = static_cast<Json::UInt64>(usedBits);
    root["feasible"] = feasible;
    root["requires_new_build"] = requiresNewBuild;
    if (!contractSnapshot.empty()) root["contract_snapshot"] = contractSnapshot;
    Json::Value entries(Json::arrayValue);
    for (const auto& entry : this->entries) entries.append(EntryJson(entry));
    root["entries"] = entries;
    Json::Value groups(Json::arrayValue);
    for (const auto& group : this->groups) groups.append(GroupJson(group));
    root["groups"] = groups;
    Json::Value missing(Json::arrayValue);
    for (const auto& signal : missingRequired) missing.append(signal);
    root["missing_required"] = missing;
    Json::Value diagnostics(Json::arrayValue);
    for (const auto& message : this->diagnostics) diagnostics.append(message);
    root["diagnostics"] = diagnostics;
    Json::StreamWriterBuilder writer;
    writer["indentation"] = "  ";
    return Json::writeString(writer, root) + "\n";
}

bool ProbePlanner::Plan(const std::vector<ProbeRequirement>& requirements,
                        const std::vector<ProbeCandidate>& candidates,
                        std::size_t capacityBits,
                        ProbePlan& result,
                        std::string& error)
{
    error.clear();
    result = ProbePlan();
    result.capacityBits = capacityBits;
    if (capacityBits == 0) {
        error = "probe plan capacityBits must be greater than zero";
        return false;
    }

    std::map<std::string, ProbeCandidate> candidateBySignal;
    std::set<std::string> conflictingCandidates;
    for (const auto& candidate : candidates) {
        if (candidate.signal.empty()) continue;
        auto it = candidateBySignal.find(candidate.signal);
        if (it == candidateBySignal.end()) {
            candidateBySignal.emplace(candidate.signal, candidate);
        } else {
            // Duplicate source records must not make selection depend on input order.
            const ProbeCandidate& current = it->second;
            if (candidate.visible != current.visible || candidate.knownWidth != current.knownWidth ||
                candidate.width != current.width || candidate.clockDomain != current.clockDomain ||
                candidate.excluded != current.excluded || candidate.alreadySelected != current.alreadySelected) {
                conflictingCandidates.insert(candidate.signal);
            }
        }
    }

    std::map<std::string, ProbeRequirement> requirementBySignal;
    std::set<std::string> conflictingRequirements;
    for (const auto& requirement : requirements) {
        if (requirement.signal.empty()) continue;
        auto [it, inserted] = requirementBySignal.emplace(requirement.signal, requirement);
        if (!inserted) {
            it->second.required = it->second.required || requirement.required;
            it->second.priority = std::max(it->second.priority, requirement.priority);
            if (it->second.reason.empty() || (!requirement.reason.empty() && requirement.reason < it->second.reason))
                it->second.reason = requirement.reason;
            if (it->second.kind.empty() || KindRank(requirement.kind) < KindRank(it->second.kind) ||
                (KindRank(requirement.kind) == KindRank(it->second.kind) && requirement.kind < it->second.kind)) {
                it->second.kind = requirement.kind;
            }
            if (!it->second.clockDomain.empty() && !requirement.clockDomain.empty() &&
                it->second.clockDomain != requirement.clockDomain) conflictingRequirements.insert(requirement.signal);
            if (it->second.width != 0 && requirement.width != 0 && it->second.width != requirement.width)
                conflictingRequirements.insert(requirement.signal);
            if (it->second.clockDomain.empty() || (!requirement.clockDomain.empty() &&
                requirement.clockDomain < it->second.clockDomain)) it->second.clockDomain = requirement.clockDomain;
            if (it->second.width == 0 || (requirement.width != 0 && requirement.width < it->second.width))
                it->second.width = requirement.width;
        }
    }

    std::vector<ProbeRequirement> ordered;
    ordered.reserve(requirementBySignal.size());
    for (const auto& item : requirementBySignal) ordered.push_back(item.second);
    std::stable_sort(ordered.begin(), ordered.end(), [](const auto& left, const auto& right) {
        if (left.required != right.required) return left.required > right.required;
        if (left.priority != right.priority) return left.priority > right.priority;
        if (KindRank(left.kind) != KindRank(right.kind)) return KindRank(left.kind) < KindRank(right.kind);
        return left.signal < right.signal;
    });

    // The initial planner targets the existing single-clock debug core.
    std::string selectedClockDomain;
    for (const auto& requirement : ordered) {
        ProbePlanEntry entry;
        entry.signal = requirement.signal;
        entry.reason = requirement.reason;
        entry.kind = requirement.kind;
        entry.clockDomain = requirement.clockDomain;
        entry.groupKey = (entry.clockDomain.empty() ? "<unknown-clock>" : entry.clockDomain) +
                         "/" + (entry.kind.empty() ? "auxiliary" : entry.kind);
        entry.width = requirement.width;
        entry.priority = requirement.priority;
        entry.required = requirement.required;

        const auto candidateIt = candidateBySignal.find(requirement.signal);
        if (conflictingRequirements.count(requirement.signal)) {
            entry.diagnostic = "conflicting requirement metadata";
        } else if (conflictingCandidates.count(requirement.signal)) {
            entry.diagnostic = "conflicting candidate metadata";
        } else if (candidateIt == candidateBySignal.end()) {
            entry.diagnostic = "signal is not visible in the current design";
        } else {
            const ProbeCandidate& candidate = candidateIt->second;
            if (!candidate.visible) entry.diagnostic = "signal is not visible in the current design";
            else if (candidate.excluded) entry.diagnostic = "signal is excluded by the current plan";
            else if (!candidate.knownWidth || candidate.width == 0) entry.diagnostic = "signal width is unknown";
            else if (requirement.width != 0 && requirement.width != candidate.width)
                entry.diagnostic = "signal width does not match requirement";
            else if (!requirement.clockDomain.empty() && !candidate.clockDomain.empty() &&
                     requirement.clockDomain != candidate.clockDomain) {
                entry.diagnostic = "clock domain mismatch: candidate=" + candidate.clockDomain;
            } else {
                entry.width = candidate.width;
                entry.clockDomain = candidate.clockDomain.empty()
                    ? requirement.clockDomain : candidate.clockDomain;
                if (entry.clockDomain.empty()) {
                    entry.diagnostic = "clock domain is unknown";
                } else if (!selectedClockDomain.empty() && entry.clockDomain != selectedClockDomain) {
                    entry.diagnostic = "single-clock core cannot combine clock domains";
                } else if (entry.width <= capacityBits - result.usedBits) {
                    entry.selected = true;
                    selectedClockDomain = entry.clockDomain;
                    entry.needsNewBuild = !candidate.alreadySelected;
                    result.usedBits += entry.width;
                    result.requiresNewBuild = result.requiresNewBuild || entry.needsNewBuild;
                } else {
                    entry.diagnostic = "probe budget exceeded";
                }
            }
        }

        if (entry.required && !entry.selected) {
            result.feasible = false;
            result.missingRequired.push_back(entry.signal);
        }
        result.entries.push_back(std::move(entry));
    }

    if (!result.feasible) {
        std::ostringstream message;
        message << "required probes violate visibility, metadata, clock or " << capacityBits << "-bit budget constraints";
        result.diagnostics.push_back(message.str());
    }
    std::map<std::string, ProbePlanGroup> groups;
    for (const auto& entry : result.entries) {
        auto& group = groups[entry.groupKey];
        group.groupKey = entry.groupKey;
        group.clockDomain = entry.clockDomain;
        group.kind = entry.kind.empty() ? "auxiliary" : entry.kind;
        ++group.entryCount;
        if (entry.selected) group.selectedBits += entry.width;
    }
    for (const auto& item : groups) result.groups.push_back(item.second);
    return true;
}

} // namespace debug
} // namespace sigflow
