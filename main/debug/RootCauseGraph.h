#pragma once

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "DebugContract.h"

namespace sigflow {
namespace debug {

struct SignalLocation {
    std::string signalName;
    std::string sourcePath;
    int sourceLine = 0;
    std::string sftreePath;
    std::string canvasId;
    std::string clockDomain;
    std::size_t fanout = 0;
    bool isInput = false;
    bool isProbe = false;
};

struct RootCauseCandidate {
    SignalLocation location;
    std::size_t graphDistance = 0;
    double score = 0.0;
    std::string reason;
};

class SignalSourceMap {
public:
    void Add(SignalLocation location);
    const SignalLocation* Find(const std::string& signalName) const;
    std::vector<std::string> InputSignals() const;
    bool LoadJson(const std::string& path, std::string& error);
    std::size_t Size() const;

private:
    std::vector<SignalLocation> locations_;
};

class DependencyGraph {
public:
    void AddNode(SignalLocation location);
    void AddDependency(const std::string& source, const std::string& dependent);
    bool LoadJson(const std::string& path, std::string& error);
    std::vector<std::pair<std::string, std::size_t>> UpstreamWithDistance(
        const std::string& signalName, std::size_t maxDepth = 4) const;
    std::vector<std::string> Upstream(const std::string& signalName,
                                      std::size_t maxDepth = 4) const;
    std::size_t NodeCount() const;

private:
    struct Edge { std::string source; std::string dependent; };
    std::vector<SignalLocation> nodes_;
    std::vector<Edge> edges_;
};

class RootCauseAnalyzer {
public:
    static std::vector<RootCauseCandidate> Rank(
        const std::string& firstDifferenceSignal,
        const DependencyGraph& graph,
        const SignalSourceMap& sourceMap,
        std::size_t maxDepth = 4,
        std::size_t maxCandidates = 8);
};

} // namespace debug
} // namespace sigflow
