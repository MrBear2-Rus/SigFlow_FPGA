#include "StimulusParser.h"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <regex>
#include <wx/filename.h>

StimulusParser::StimulusParser() {}
StimulusParser::~StimulusParser() {}

bool StimulusParser::Parse(const wxString& filePath, TestbenchInfo& result)
{
    std::ifstream file(filePath.ToStdString());
    if (!file.is_open()) {
        m_lastError = wxString::Format("无法打开文件: %s", filePath);
        return false;
    }

    std::string code((std::istreambuf_iterator<char>(file)),
                      std::istreambuf_iterator<char>());
    file.close();

    return ParseCode(code, result);
}

bool StimulusParser::ParseCode(const std::string& code, TestbenchInfo& result)
{
    std::string cleaned = Preprocess(code);

    // 提取模块名
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

std::string StimulusParser::Preprocess(const std::string& code)
{
    std::string result;
    result.reserve(code.size());

    // 去除单行注释和多行注释
    bool inLineComment = false;
    bool inBlockComment = false;
    bool inString = false;

    for (size_t i = 0; i < code.size(); ++i) {
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

void StimulusParser::ParseSignalDeclarations(const std::string& code, TestbenchInfo& info)
{
    // 匹配 reg 声明: reg signal_name; 或 reg [N:M] signal_name;
    std::regex regRe(R"(reg\s+(?:\[\s*\d+\s*:\s*\d+\s*\]\s*)?(\w+)\s*;)");
    auto regBegin = std::sregex_iterator(code.begin(), code.end(), regRe);
    auto regEnd = std::sregex_iterator();
    for (auto it = regBegin; it != regEnd; ++it) {
        info.regSignals.push_back((*it)[1].str());
    }

    // 匹配 wire 声明: wire signal_name; 或 wire [N:M] signal_name;
    std::regex wireRe(R"(wire\s+(?:\[\s*\d+\s*:\s*\d+\s*\]\s*)?(\w+)\s*;)");
    auto wireBegin = std::sregex_iterator(code.begin(), code.end(), wireRe);
    auto wireEnd = std::sregex_iterator();
    for (auto it = wireBegin; it != wireEnd; ++it) {
        info.wireSignals.push_back((*it)[1].str());
    }
}

void StimulusParser::ParseModuleInstantiation(const std::string& code, TestbenchInfo& info)
{
    // 匹配模块实例化: module_name instance_name( .port(signal), ... );
    // 支持多行格式
    std::regex instRe(R"((\w+)\s+(\w+)\s*\(([\s\S]*?)\)\s*;)");
    auto instBegin = std::sregex_iterator(code.begin(), code.end(), instRe);
    auto instEnd = std::sregex_iterator();

    for (auto it = instBegin; it != instEnd; ++it) {
        std::string moduleName = (*it)[1].str();
        std::string instName = (*it)[2].str();
        std::string ports = (*it)[3].str();

        // 跳过 Verilog 关键字（不是实例化）
        static const std::vector<std::string> keywords = {
            "module", "initial", "always", "assign", "begin", "end",
            "if", "else", "case", "for", "while", "reg", "wire",
            "input", "output", "inout", "parameter", "integer"
        };
        bool isKeyword = false;
        for (const auto& kw : keywords) {
            if (moduleName == kw) { isKeyword = true; break; }
        }
        if (isKeyword) continue;

        // 检查是否包含 .port(signal) 格式
        if (ports.find('.') == std::string::npos) continue;

        info.topModuleName = moduleName;
        info.topInstanceName = instName;

        // 解析端口映射: .port_name(signal_name)
        std::regex portRe(R"(\.(\w+)\s*\(\s*(\w+)\s*\))");
        auto portBegin = std::sregex_iterator(ports.begin(), ports.end(), portRe);
        auto portEnd = std::sregex_iterator();
        for (auto pit = portBegin; pit != portEnd; ++pit) {
            TBPortMapping mapping;
            mapping.portName = (*pit)[1].str();
            mapping.signalName = (*pit)[2].str();
            info.portMappings.push_back(mapping);
        }

        break; // 只取第一个实例化（通常是 DUT）
    }
}

void StimulusParser::ParseInitialBlocks(const std::string& code, TestbenchInfo& info)
{
    // 查找所有 initial begin ... end 块
    size_t searchPos = 0;
    while (true) {
        size_t initialPos = code.find("initial", searchPos);
        if (initialPos == std::string::npos) break;

        // 确保不是单词的一部分
        if (initialPos > 0 && std::isalnum(code[initialPos - 1])) {
            searchPos = initialPos + 7;
            continue;
        }

        // 找到 begin
        size_t beginPos = code.find("begin", initialPos + 7);
        if (beginPos == std::string::npos) break;

        // 匹配配对的 end（处理嵌套 begin/end）
        int depth = 1;
        size_t pos = beginPos + 5;
        size_t endPos = std::string::npos;

        while (pos < code.size() && depth > 0) {
            // 简化的 begin/end 匹配
            size_t nextBegin = code.find("begin", pos);
            size_t nextEnd = code.find("end", pos);

            if (nextEnd == std::string::npos) break;

            if (nextBegin != std::string::npos && nextBegin < nextEnd) {
                // 确保 "begin" 不是单词的一部分（如 "beginner"）
                bool isWord = true;
                if (nextBegin > 0 && std::isalnum(code[nextBegin - 1])) isWord = false;
                if (nextBegin + 5 < code.size() && std::isalnum(code[nextBegin + 5])) isWord = false;
                if (isWord) depth++;
                pos = nextBegin + 5;
            } else {
                // 确保 "end" 不是 "endmodule" 等
                bool isEnd = true;
                if (nextEnd > 0 && std::isalnum(code[nextEnd - 1])) isEnd = false;
                if (nextEnd + 3 < code.size() && std::isalnum(code[nextEnd + 3])) isEnd = false;
                if (isEnd) {
                    depth--;
                    if (depth == 0) {
                        endPos = nextEnd;
                        break;
                    }
                }
                pos = nextEnd + 3;
            }
        }

        if (endPos == std::string::npos) {
            searchPos = beginPos + 5;
            continue;
        }

        std::string blockContent = code.substr(beginPos + 5, endPos - beginPos - 5);
        InitialBlock block = ParseSingleInitialBlock(blockContent);
        info.initialBlocks.push_back(block);

        searchPos = endPos + 3;
    }
}

InitialBlock StimulusParser::ParseSingleInitialBlock(const std::string& blockContent)
{
    InitialBlock block;

    // 检查是否包含 forever（时钟定义）
    size_t foreverPos = blockContent.find("forever");
    if (foreverPos != std::string::npos) {
        block.hasClock = true;

        // 解析 forever begin #N clk = ~clk; end
        // 或 forever #N clk = ~clk;
        std::regex clockRe(R"(#(\d+)\s+(\w+)\s*=\s*~\s*\2)");
        std::smatch clockMatch;
        std::string foreverContent = blockContent.substr(foreverPos);
        if (std::regex_search(foreverContent, clockMatch, clockRe)) {
            block.clock.halfPeriod = std::stoull(clockMatch[1].str());
            block.clock.signalName = clockMatch[2].str();
        }

        // 提取 forever 之前的初始赋值
        std::string preForever = blockContent.substr(0, foreverPos);
        std::regex assignRe(R"((\w+)\s*=\s*(\d+|'[bhd]\w+)\s*;)");
        auto it = std::sregex_iterator(preForever.begin(), preForever.end(), assignRe);
        auto end = std::sregex_iterator();
        for (; it != end; ++it) {
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

    // 普通 initial 块：逐行解析
    std::istringstream stream(blockContent);
    std::string line;
    while (std::getline(stream, line)) {
        // trim
        size_t start = line.find_first_not_of(" \t\r\n");
        if (start == std::string::npos) continue;
        line = line.substr(start);
        size_t endTrim = line.find_last_not_of(" \t\r\n;");
        if (endTrim != std::string::npos)
            line = line.substr(0, endTrim + 1);
        if (line.empty()) continue;

        // 延迟语句: #N 或 #N;
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

        // 边沿等待: @(posedge signal) 或 @(negedge signal)
        std::regex edgeRe(R"(@\s*\(\s*(posedge|negedge)\s+(\w+)\s*\))");
        std::smatch edgeMatch;
        if (std::regex_search(line, edgeMatch, edgeRe)) {
            StimulusEvent evt;
            evt.type = (edgeMatch[1].str() == "posedge") ?
                StimulusEventType::PosedgeWait : StimulusEventType::NegedgeWait;
            evt.edgeSignal = edgeMatch[2].str();
            evt.rawText = line;
            block.events.push_back(evt);

            // 同一行可能还有赋值 @(posedge clk); signal = value;
            // 但通常在下一行，所以跳过
            continue;
        }

        // $finish
        if (line.find("$finish") != std::string::npos) {
            StimulusEvent evt;
            evt.type = StimulusEventType::Finish;
            evt.rawText = line;
            block.events.push_back(evt);
            continue;
        }

        // $display
        if (line.find("$display") != std::string::npos) {
            StimulusEvent evt;
            evt.type = StimulusEventType::Display;
            evt.rawText = line;
            block.events.push_back(evt);
            continue;
        }

        // 赋值语句: signal = value 或 signal <= value
        StimulusEvent assignEvt = ParseAssignment(line);
        if (assignEvt.type == StimulusEventType::SignalAssign) {
            block.events.push_back(assignEvt);
        }
    }

    return block;
}

StimulusEvent StimulusParser::ParseAssignment(const std::string& line)
{
    StimulusEvent evt;
    evt.type = StimulusEventType::SignalAssign;

    // 匹配 signal = value 或 signal <= value
    // 支持: x = 0, x = 1, x = 8'hFF, x = 4'b1010
    std::regex assignRe(R"((\w+)\s*(?:<=|=)\s*(\d+'[bhd]\w+|\d+|'[bhd]\w+))");
    std::smatch match;
    if (std::regex_search(line, match, assignRe)) {
        evt.signalName = match[1].str();
        evt.value = match[2].str();
        evt.rawText = line;
        return evt;
    }

    // 没匹配到，返回无效事件
    evt.type = StimulusEventType::Display;
    return evt;
}

uint64_t StimulusParser::EstimateMaxSimTime(const TestbenchInfo& info)
{
    uint64_t totalTime = 0;

    for (const auto& block : info.initialBlocks) {
        if (block.hasClock) continue; // 时钟块不计入

        uint64_t blockTime = 0;
        for (const auto& evt : block.events) {
            if (evt.type == StimulusEventType::Delay) {
                blockTime += evt.delayValue;
            } else if (evt.type == StimulusEventType::PosedgeWait ||
                       evt.type == StimulusEventType::NegedgeWait) {
                // 边沿等待：估算为一个时钟周期
                for (const auto& b : info.initialBlocks) {
                    if (b.hasClock) {
                        blockTime += b.clock.halfPeriod * 2;
                        break;
                    }
                }
            }
        }
        if (blockTime > totalTime) totalTime = blockTime;
    }

    // 至少运行一些额外步骤
    if (totalTime == 0) totalTime = 200;
    return totalTime;
}
