#include "StimulusModel.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>

namespace eda {
namespace sim {

StimulusParser::StimulusParser() = default;
StimulusParser::~StimulusParser() = default;

bool StimulusParser::Parse(const std::string& filePath, TestbenchInfo& result) {
    std::ifstream file{std::filesystem::path(filePath)};
    if (!file.is_open()) {
        lastError_ = "Cannot open file: " + filePath;
        return false;
    }
    std::string code((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    file.close();
    return ParseCode(code, result);
}

bool StimulusParser::ParseCode(const std::string& code, TestbenchInfo& result) {
    const std::string cleaned = Preprocess(code);

    std::regex moduleRe(R"(module\s+(\w+))");
    std::smatch moduleMatch;
    if (std::regex_search(cleaned, moduleMatch, moduleRe)) {
        result.moduleName = moduleMatch[1].str();
    }

    ParseSignalDeclarations(cleaned, result);
    ParseModuleInstantiation(cleaned, result);
    ParseInitialBlocks(cleaned, result);
    result.maxSimTime = EstimateMaxSimTime(result);
    return true;
}

std::string StimulusParser::Preprocess(const std::string& code) {
    std::string result;
    result.reserve(code.size());

    bool inLineComment = false;
    bool inBlockComment = false;
    bool inString = false;

    for (std::size_t i = 0; i < code.size(); ++i) {
        if (inBlockComment) {
            if (i + 1 < code.size() && code[i] == '*' && code[i + 1] == '/') {
                inBlockComment = false;
                ++i;
            }
            continue;
        }
        if (inLineComment) {
            if (code[i] == '\n') {
                inLineComment = false;
                result += '\n';
            }
            continue;
        }
        if (code[i] == '"') {
            inString = !inString;
            result += code[i];
            continue;
        }
        if (!inString && i + 1 < code.size()) {
            if (code[i] == '/' && code[i + 1] == '/') {
                inLineComment = true;
                continue;
            }
            if (code[i] == '/' && code[i + 1] == '*') {
                inBlockComment = true;
                ++i;
                continue;
            }
        }
        result += code[i];
    }
    return result;
}

void StimulusParser::ParseSignalDeclarations(const std::string& code, TestbenchInfo& info) {
    std::regex regRe(R"(reg\s+(?:\[\s*\d+\s*:\s*\d+\s*\]\s*)?(\w+)\s*;)");
    for (auto it = std::sregex_iterator(code.begin(), code.end(), regRe);
         it != std::sregex_iterator(); ++it) {
        info.regSignals.push_back((*it)[1].str());
    }

    std::regex wireRe(R"(wire\s+(?:\[\s*\d+\s*:\s*\d+\s*\]\s*)?(\w+)\s*;)");
    for (auto it = std::sregex_iterator(code.begin(), code.end(), wireRe);
         it != std::sregex_iterator(); ++it) {
        info.wireSignals.push_back((*it)[1].str());
    }
}

void StimulusParser::ParseModuleInstantiation(const std::string& code, TestbenchInfo& info) {
    std::regex instRe(R"((\w+)\s+(\w+)\s*\(([\s\S]*?)\)\s*;)");
    for (auto it = std::sregex_iterator(code.begin(), code.end(), instRe);
         it != std::sregex_iterator(); ++it) {
        const std::string moduleName = (*it)[1].str();
        const std::string instName = (*it)[2].str();
        const std::string ports = (*it)[3].str();

        static const std::vector<std::string> keywords = {
            "module", "initial", "always", "assign", "begin", "end",
            "if", "else", "case", "for", "while", "reg", "wire",
            "input", "output", "inout", "parameter", "integer"};
        bool isKeyword = false;
        for (const auto& keyword : keywords) {
            if (moduleName == keyword) { isKeyword = true; break; }
        }
        if (isKeyword) continue;
        if (ports.find('.') == std::string::npos) continue;

        info.topModuleName = moduleName;
        info.topInstanceName = instName;

        std::regex portRe(R"(\.(\w+)\s*\(\s*(\w+)\s*\))");
        for (auto pit = std::sregex_iterator(ports.begin(), ports.end(), portRe);
             pit != std::sregex_iterator(); ++pit) {
            TBPortMapping mapping;
            mapping.portName = (*pit)[1].str();
            mapping.signalName = (*pit)[2].str();
            info.portMappings.push_back(mapping);
        }
        break;
    }
}

void StimulusParser::ParseInitialBlocks(const std::string& code, TestbenchInfo& info) {
    std::size_t searchPos = 0;
    while (true) {
        const std::size_t initialPos = code.find("initial", searchPos);
        if (initialPos == std::string::npos) break;
        if (initialPos > 0 && std::isalnum(static_cast<unsigned char>(code[initialPos - 1]))) {
            searchPos = initialPos + 7;
            continue;
        }
        const std::size_t beginPos = code.find("begin", initialPos + 7);
        if (beginPos == std::string::npos) break;

        int depth = 1;
        std::size_t pos = beginPos + 5;
        std::size_t endPos = std::string::npos;
        while (pos < code.size() && depth > 0) {
            const std::size_t nextBegin = code.find("begin", pos);
            const std::size_t nextEnd = code.find("end", pos);
            if (nextEnd == std::string::npos) break;

            if (nextBegin != std::string::npos && nextBegin < nextEnd) {
                bool isWord = true;
                if (nextBegin > 0 && std::isalnum(static_cast<unsigned char>(code[nextBegin - 1]))) isWord = false;
                if (nextBegin + 5 < code.size() && std::isalnum(static_cast<unsigned char>(code[nextBegin + 5]))) isWord = false;
                if (isWord) depth++;
                pos = nextBegin + 5;
            } else {
                bool isEnd = true;
                if (nextEnd > 0 && std::isalnum(static_cast<unsigned char>(code[nextEnd - 1]))) isEnd = false;
                if (nextEnd + 3 < code.size() && std::isalnum(static_cast<unsigned char>(code[nextEnd + 3]))) isEnd = false;
                if (isEnd) {
                    depth--;
                    if (depth == 0) { endPos = nextEnd; break; }
                }
                pos = nextEnd + 3;
            }
        }
        if (endPos == std::string::npos) {
            searchPos = beginPos + 5;
            continue;
        }
        const std::string blockContent = code.substr(beginPos + 5, endPos - beginPos - 5);
        info.initialBlocks.push_back(ParseSingleInitialBlock(blockContent));
        searchPos = endPos + 3;
    }
}

InitialBlock StimulusParser::ParseSingleInitialBlock(const std::string& blockContent) {
    InitialBlock block;

    const std::size_t foreverPos = blockContent.find("forever");
    if (foreverPos != std::string::npos) {
        block.hasClock = true;

        std::regex clockRe(R"(#(\d+)\s+(\w+)\s*=\s*~\s*\2)");
        std::smatch clockMatch;
        const std::string foreverContent = blockContent.substr(foreverPos);
        if (std::regex_search(foreverContent, clockMatch, clockRe)) {
            block.clock.halfPeriod = std::stoull(clockMatch[1].str());
            block.clock.signalName = clockMatch[2].str();
        }

        const std::string preForever = blockContent.substr(0, foreverPos);
        std::regex assignRe(R"((\w+)\s*=\s*(\d+|'[bhd]\w+)\s*;)");
        for (auto it = std::sregex_iterator(preForever.begin(), preForever.end(), assignRe);
             it != std::sregex_iterator(); ++it) {
            StimulusEvent evt;
            evt.type = StimulusEventType::SignalAssign;
            evt.signalName = (*it)[1].str();
            evt.value = (*it)[2].str();
            block.events.push_back(evt);
            if (evt.signalName == block.clock.signalName) {
                block.clock.initialValue = evt.value;
            }
        }
        return block;
    }

    std::istringstream stream(blockContent);
    std::string line;
    while (std::getline(stream, line)) {
        const std::size_t start = line.find_first_not_of(" \t\r\n");
        if (start == std::string::npos) continue;
        line = line.substr(start);
        const std::size_t endTrim = line.find_last_not_of(" \t\r\n;");
        if (endTrim != std::string::npos) line = line.substr(0, endTrim + 1);
        if (line.empty()) continue;

        std::regex delayOnlyRe(R"(^#(\d+)\s*;?\s*$)");
        std::smatch delayMatch;
        if (std::regex_match(line, delayMatch, delayOnlyRe)) {
            StimulusEvent evt;
            evt.type = StimulusEventType::Delay;
            evt.delayValue = std::stoull(delayMatch[1].str());
            evt.rawText = line;
            block.events.push_back(evt);
            continue;
        }

        std::regex edgeRe(R"(@\s*\(\s*(posedge|negedge)\s+(\w+)\s*\))");
        std::smatch edgeMatch;
        if (std::regex_search(line, edgeMatch, edgeRe)) {
            StimulusEvent evt;
            evt.type = (edgeMatch[1].str() == "posedge") ? StimulusEventType::PosedgeWait
                                                         : StimulusEventType::NegedgeWait;
            evt.edgeSignal = edgeMatch[2].str();
            evt.rawText = line;
            block.events.push_back(evt);
            continue;
        }

        if (line.find("$finish") != std::string::npos) {
            StimulusEvent evt;
            evt.type = StimulusEventType::Finish;
            evt.rawText = line;
            block.events.push_back(evt);
            continue;
        }
        if (line.find("$display") != std::string::npos) {
            StimulusEvent evt;
            evt.type = StimulusEventType::Display;
            evt.rawText = line;
            block.events.push_back(evt);
            continue;
        }

        const StimulusEvent assignEvt = ParseAssignment(line);
        if (assignEvt.type == StimulusEventType::SignalAssign) {
            block.events.push_back(assignEvt);
        }
    }
    return block;
}

StimulusEvent StimulusParser::ParseAssignment(const std::string& line) {
    StimulusEvent evt;
    evt.type = StimulusEventType::SignalAssign;
    std::regex assignRe(R"((\w+)\s*(?:<=|=)\s*(\d+'[bhd]\w+|\d+|'[bhd]\w+))");
    std::smatch match;
    if (std::regex_search(line, match, assignRe)) {
        evt.signalName = match[1].str();
        evt.value = match[2].str();
        evt.rawText = line;
        return evt;
    }
    evt.type = StimulusEventType::Display;
    return evt;
}

std::uint64_t StimulusParser::EstimateMaxSimTime(const TestbenchInfo& info) {
    std::uint64_t totalTime = 0;
    for (const auto& block : info.initialBlocks) {
        if (block.hasClock) continue;
        std::uint64_t blockTime = 0;
        for (const auto& evt : block.events) {
            if (evt.type == StimulusEventType::Delay) {
                blockTime += evt.delayValue;
            } else if (evt.type == StimulusEventType::PosedgeWait ||
                       evt.type == StimulusEventType::NegedgeWait) {
                for (const auto& other : info.initialBlocks) {
                    if (other.hasClock) { blockTime += other.clock.halfPeriod * 2; break; }
                }
            }
        }
        if (blockTime > totalTime) totalTime = blockTime;
    }
    if (totalTime == 0) totalTime = 200;
    return totalTime;
}

} // namespace sim
} // namespace eda
