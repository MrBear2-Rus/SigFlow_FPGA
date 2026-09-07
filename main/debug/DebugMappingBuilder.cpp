#include "DebugMappingBuilder.h"

#include "RootCauseGraph.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <json/json.h>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <unordered_set>

namespace sigflow {
namespace debug {
namespace {

struct Symbol {
    SignalLocation location;
    std::size_t line = 0;
};

const std::unordered_set<std::string> kKeywords = {
    "always", "always_ff", "always_comb", "assign", "begin", "case", "end",
    "else", "for", "if", "module", "negedge", "posedge", "reg", "wire",
    "logic", "input", "output", "inout", "integer", "parameter", "localparam",
    "signed", "unsigned", "true", "false"
};

std::string LeafName(const std::string& value) {
    const std::size_t dot = value.find_last_of("./");
    return dot == std::string::npos ? value : value.substr(dot + 1);
}

bool IsIdentifier(const std::string& value) {
    if (value.empty() || !(std::isalpha(static_cast<unsigned char>(value.front())) || value.front() == '_')) {
        return false;
    }
    return std::all_of(value.begin() + 1, value.end(), [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
    });
}

void AddSymbol(std::map<std::string, Symbol>& symbols, const std::string& name,
               const std::string& sourcePath, std::size_t line, bool isInput,
               const std::string& clockDomain) {
    if (!IsIdentifier(name) || kKeywords.count(name) != 0) return;
    auto& symbol = symbols[name];
    if (symbol.location.signalName.empty()) symbol.location.signalName = name;
    if (symbol.location.sourcePath.empty()) symbol.location.sourcePath = sourcePath;
    if (symbol.location.sourceLine == 0) symbol.location.sourceLine = static_cast<int>(line);
    if (symbol.location.clockDomain.empty()) symbol.location.clockDomain = clockDomain;
    symbol.location.isInput = symbol.location.isInput || isInput;
    symbol.line = symbol.location.sourceLine > 0 ? static_cast<std::size_t>(symbol.location.sourceLine) : line;
}

std::string StripLineComment(std::string line) {
    const std::size_t comment = line.find("//");
    if (comment != std::string::npos) line.resize(comment);
    return line;
}

void ParseSource(const std::string& path, const std::string& clockDomain,
                 std::map<std::string, Symbol>& symbols,
                 std::set<std::pair<std::string, std::string>>& edges) {
    std::ifstream input(path);
    if (!input) return;
    const std::regex portDecl(R"(\b(input|output|inout)\b(?:\s+(?:wire|reg|logic))?(?:\s*\[[^\]]+\])?\s+([A-Za-z_]\w*))");
    const std::regex signalDecl(R"(\b(?:wire|reg|logic)\b(?:\s*\[[^\]]+\])?\s+([A-Za-z_]\w*))");
    const std::regex assignment(R"(\b([A-Za-z_]\w*)\s*(?:<=|=)\s*([^;]+))");
    const std::regex identifier(R"([A-Za-z_]\w*)");
    std::string line;
    std::size_t lineNumber = 0;
    while (std::getline(input, line)) {
        ++lineNumber;
        line = StripLineComment(std::move(line));
        for (std::sregex_iterator it(line.begin(), line.end(), portDecl), end; it != end; ++it) {
            AddSymbol(symbols, (*it)[2].str(), path, lineNumber, (*it)[1].str() == "input", clockDomain);
        }
        for (std::sregex_iterator it(line.begin(), line.end(), signalDecl), end; it != end; ++it) {
            AddSymbol(symbols, (*it)[1].str(), path, lineNumber, false, clockDomain);
        }
        std::smatch match;
        if (!std::regex_search(line, match, assignment)) continue;
        const std::string dependent = match[1].str();
        AddSymbol(symbols, dependent, path, lineNumber, false, clockDomain);
        const std::string rhs = match[2].str();
        for (std::sregex_iterator it(rhs.begin(), rhs.end(), identifier), end; it != end; ++it) {
            const std::string source = (*it).str();
            const std::size_t position = static_cast<std::size_t>(it->position());
            if (position > 0 && (std::isdigit(static_cast<unsigned char>(rhs[position - 1])) ||
                                 rhs[position - 1] == '\'')) continue;
            if (source != dependent && kKeywords.count(source) == 0) {
                AddSymbol(symbols, source, path, lineNumber, false, clockDomain);
                edges.insert({source, dependent});
            }
        }
    }
}

void ApplyContract(const std::string& topModule, const DebugContract& contract,
                   const std::string& defaultClock, std::map<std::string, Symbol>& symbols) {
    for (const auto& probe : contract.probes) {
        const std::string name = LeafName(probe.path.empty() ? probe.id : probe.path);
        AddSymbol(symbols, name, {}, 0, false,
                  probe.clockDomain.empty() ? defaultClock : probe.clockDomain);
        auto& symbol = symbols[name];
        symbol.location.isProbe = true;
        symbol.location.sftreePath = probe.path.empty() ? topModule + "." + name : probe.path;
        symbol.location.canvasId = probe.id.empty() ? name : probe.id;
    }
}

Json::Value ToJson(const std::map<std::string, Symbol>& symbols) {
    Json::Value value(Json::arrayValue);
    for (const auto& [name, symbol] : symbols) {
        const auto& location = symbol.location;
        Json::Value item;
        item["signal"] = name;
        item["source_path"] = location.sourcePath;
        item["source_line"] = location.sourceLine;
        item["sftree_path"] = location.sftreePath.empty() ? name : location.sftreePath;
        item["canvas_id"] = location.canvasId.empty() ? name : location.canvasId;
        item["clock_domain"] = location.clockDomain;
        item["fanout"] = Json::UInt64(location.fanout);
        item["direction"] = location.isInput ? "input" : "internal";
        item["probe"] = location.isProbe;
        value.append(item);
    }
    return value;
}

} // namespace

bool DebugMappingBuilder::Generate(const std::string& topModule,
                                   const std::vector<std::string>& sourceFiles,
                                   const DebugContract& contract,
                                   const std::string& outputDirectory,
                                   DebugMappingBuildResult& result,
                                   std::string& error) {
    result = DebugMappingBuildResult();
    if (topModule.empty() || outputDirectory.empty()) {
        error = "mapping generation requires top module and output directory";
        return false;
    }
    std::error_code ec;
    std::filesystem::create_directories(outputDirectory, ec);
    if (ec) {
        error = "cannot create mapping directory: " + outputDirectory;
        return false;
    }
    std::map<std::string, Symbol> symbols;
    std::set<std::pair<std::string, std::string>> edges;
    const std::string clock = LeafName(contract.sampleClock.signal);
    for (const auto& source : sourceFiles) ParseSource(source, clock, symbols, edges);
    ApplyContract(topModule, contract, clock, symbols);
    for (const auto& [source, dependent] : edges) {
        auto it = symbols.find(source);
        if (it != symbols.end()) ++it->second.location.fanout;
    }
    Json::Value sourceRoot;
    sourceRoot["schema_version"] = "1.0";
    sourceRoot["top_module"] = topModule;
    sourceRoot["signals"] = ToJson(symbols);
    Json::Value graphRoot;
    graphRoot["schema_version"] = "1.0";
    graphRoot["top_module"] = topModule;
    Json::Value nodes(Json::arrayValue);
    for (const auto& [name, symbol] : symbols) {
        Json::Value node;
        node["signal"] = name;
        node["clock_domain"] = symbol.location.clockDomain;
        node["fanout"] = Json::UInt64(symbol.location.fanout);
        nodes.append(node);
    }
    Json::Value dependencies(Json::arrayValue);
    for (const auto& [source, dependent] : edges) {
        Json::Value edge;
        edge["source"] = source;
        edge["dependent"] = dependent;
        dependencies.append(edge);
    }
    graphRoot["nodes"] = nodes;
    graphRoot["dependencies"] = dependencies;
    Json::StreamWriterBuilder writer;
    writer["indentation"] = "  ";
    std::ofstream sourceOutput(std::filesystem::path(outputDirectory) / "signal-map.json", std::ios::trunc);
    std::ofstream graphOutput(std::filesystem::path(outputDirectory) / "dependency-graph.json", std::ios::trunc);
    if (!sourceOutput || !graphOutput) {
        error = "cannot write generated mapping sidecars";
        return false;
    }
    sourceOutput << Json::writeString(writer, sourceRoot);
    graphOutput << Json::writeString(writer, graphRoot);
    result.mappedSignals = symbols.size();
    result.graphNodes = symbols.size();
    result.graphEdges = edges.size();
    result.inputSignals = std::count_if(symbols.begin(), symbols.end(), [](const auto& item) {
        return item.second.location.isInput && item.second.location.isProbe;
    });
    return true;
}

} // namespace debug
} // namespace sigflow
