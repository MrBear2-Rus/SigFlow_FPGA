#include "RootCauseGraph.h"

#include <algorithm>
#include <fstream>
#include <json/json.h>
#include <map>
#include <memory>
#include <queue>
#include <set>
#include <iterator>
#include <utility>

namespace sigflow {
namespace debug {

void SignalSourceMap::Add(SignalLocation location) {
    for (auto& existing : locations_) {
        if (existing.signalName == location.signalName) {
            existing = std::move(location);
            return;
        }
    }
    locations_.push_back(std::move(location));
}

const SignalLocation* SignalSourceMap::Find(const std::string& signalName) const {
    for (const auto& location : locations_) {
        if (location.signalName == signalName) return &location;
    }
    return nullptr;
}

std::vector<std::string> SignalSourceMap::InputSignals() const {
    std::vector<std::string> result;
    for (const auto& location : locations_) {
        if (location.isInput && location.isProbe) result.push_back(location.signalName);
    }
    return result;
}

bool SignalSourceMap::LoadJson(const std::string& path, std::string& error) {
    std::ifstream input(path);
    if (!input) {
        error = "cannot open signal source map: " + path;
        return false;
    }
    Json::Value root;
    Json::CharReaderBuilder builder;
    std::string parseError;
    std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    std::string content((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    if (!reader->parse(content.data(), content.data() + content.size(), &root, &parseError) ||
        !root.isObject() || !root["signals"].isArray()) {
        error = "invalid signal source map: " + parseError;
        return false;
    }
    locations_.clear();
    for (const auto& item : root["signals"]) {
        if (!item.isObject() || !item["signal"].isString()) continue;
        SignalLocation location;
        location.signalName = item["signal"].asString();
        location.sourcePath = item.get("source_path", "").asString();
        location.sourceLine = item.get("source_line", 0).asInt();
        location.sftreePath = item.get("sftree_path", location.signalName).asString();
        location.canvasId = item.get("canvas_id", location.signalName).asString();
        location.clockDomain = item.get("clock_domain", "").asString();
        location.fanout = item.get("fanout", 0).asUInt64();
        location.isInput = item.get("direction", "").asString() == "input" ||
                           item.get("is_input", false).asBool();
        location.isProbe = item.get("probe", false).asBool();
        Add(std::move(location));
    }
    return true;
}

std::size_t SignalSourceMap::Size() const { return locations_.size(); }

void DependencyGraph::AddNode(SignalLocation location) {
    for (auto& existing : nodes_) {
        if (existing.signalName == location.signalName) {
            existing = std::move(location);
            return;
        }
    }
    nodes_.push_back(std::move(location));
}

void DependencyGraph::AddDependency(const std::string& source, const std::string& dependent) {
    if (source.empty() || dependent.empty() || source == dependent) return;
    for (const auto& edge : edges_) {
        if (edge.source == source && edge.dependent == dependent) return;
    }
    edges_.push_back({source, dependent});
}

std::vector<std::pair<std::string, std::size_t>> DependencyGraph::UpstreamWithDistance(
    const std::string& signalName, std::size_t maxDepth) const {
    std::vector<std::pair<std::string, std::size_t>> result;
    std::queue<std::pair<std::string, std::size_t>> pending;
    std::set<std::string> visited;
    pending.push({signalName, 0});
    visited.insert(signalName);
    while (!pending.empty()) {
        const auto current = pending.front();
        pending.pop();
        if (current.second >= maxDepth) continue;
        for (const auto& edge : edges_) {
            if (edge.dependent != current.first || visited.count(edge.source) != 0) continue;
            visited.insert(edge.source);
            result.push_back({edge.source, current.second + 1});
            pending.push({edge.source, current.second + 1});
        }
    }
    return result;
}

std::vector<std::string> DependencyGraph::Upstream(const std::string& signalName,
                                                   std::size_t maxDepth) const {
    std::vector<std::string> result;
    for (const auto& item : UpstreamWithDistance(signalName, maxDepth)) {
        result.push_back(item.first);
    }
    return result;
}

std::size_t DependencyGraph::NodeCount() const { return nodes_.size(); }

bool DependencyGraph::LoadJson(const std::string& path, std::string& error) {
    std::ifstream input(path);
    if (!input) {
        error = "cannot open dependency graph: " + path;
        return false;
    }
    Json::Value root;
    Json::CharReaderBuilder builder;
    std::string parseError;
    std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    std::string content((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    if (!reader->parse(content.data(), content.data() + content.size(), &root, &parseError) ||
        !root.isObject()) {
        error = "invalid dependency graph: " + parseError;
        return false;
    }
    nodes_.clear();
    edges_.clear();
    if (root["nodes"].isArray()) {
        for (const auto& item : root["nodes"]) {
            if (!item.isObject() || !item["signal"].isString()) continue;
            SignalLocation location;
            location.signalName = item["signal"].asString();
            location.clockDomain = item.get("clock_domain", "").asString();
            location.fanout = item.get("fanout", 0).asUInt64();
            AddNode(std::move(location));
        }
    }
    if (root["dependencies"].isArray()) {
        for (const auto& item : root["dependencies"]) {
            if (!item.isObject()) continue;
            AddDependency(item.get("source", "").asString(),
                          item.get("dependent", "").asString());
        }
    }
    return true;
}

std::vector<RootCauseCandidate> RootCauseAnalyzer::Rank(
    const std::string& firstDifferenceSignal,
    const DependencyGraph& graph,
    const SignalSourceMap& sourceMap,
    std::size_t maxDepth,
    std::size_t maxCandidates) {
    std::vector<RootCauseCandidate> result;
    const auto upstream = graph.UpstreamWithDistance(firstDifferenceSignal, maxDepth);
    for (const auto& upstreamItem : upstream) {
        if (result.size() >= maxCandidates) break;
        RootCauseCandidate candidate;
        const SignalLocation* location = sourceMap.Find(upstreamItem.first);
        candidate.location = location ? *location : SignalLocation{upstreamItem.first, "", 0, upstreamItem.first, upstreamItem.first, "", 0};
        candidate.graphDistance = upstreamItem.second;
        const double distanceScore = 1.0 / static_cast<double>(candidate.graphDistance);
        const double fanoutPenalty = std::min(0.25, static_cast<double>(candidate.location.fanout) / 100.0);
        const double clockBonus = candidate.location.clockDomain.empty() ? 0.0 : 0.1;
        candidate.score = std::max(0.0, 0.9 * distanceScore - fanoutPenalty + clockBonus);
        candidate.reason = "upstream distance=" + std::to_string(candidate.graphDistance);
        result.push_back(std::move(candidate));
    }
    std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
        if (left.score != right.score) return left.score > right.score;
        return left.graphDistance < right.graphDistance;
    });
    return result;
}

} // namespace debug
} // namespace sigflow
