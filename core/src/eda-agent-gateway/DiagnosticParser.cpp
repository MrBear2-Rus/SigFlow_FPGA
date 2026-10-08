#include "DiagnosticParser.h"

#include <cctype>
#include <cstdint>
#include <sstream>

namespace eda {
namespace agent {
namespace {

// 去除行首尾空白。
std::string Trim(const std::string& text) {
    std::size_t begin = 0;
    std::size_t end = text.size();
    const auto isSpace = [](unsigned char c) { return std::isspace(c) != 0; };
    while (begin < end && isSpace(static_cast<unsigned char>(text[begin]))) ++begin;
    while (end > begin && isSpace(static_cast<unsigned char>(text[end - 1]))) --end;
    return text.substr(begin, end - begin);
}

bool MapSeverity(const std::string& token, std::string& out) {
    std::string lowered;
    for (char c : token) lowered.push_back(static_cast<char>(std::tolower(c)));
    if (lowered == "error" || lowered == "fatal") {
        out = "error";
        return true;
    }
    if (lowered == "warning") {
        out = "warning";
        return true;
    }
    if (lowered == "info" || lowered == "note") {
        out = "info";
        return true;
    }
    return false;
}

// Verilator 行：%Warning-DECLFILENAME: file.sv:12: message
//            %Error: file.sv:3: message
bool ParseVerilatorLine(const std::string& line, DiagnosticParser::Parsed& out) {
    if (line.rfind("%", 0) != 0) return false;
    const std::size_t colon = line.find(':');
    if (colon == std::string::npos) return false;
    std::string levelToken = line.substr(1, colon - 1);  // Error[-CODE] / Warning[-CODE] / Info
    std::string level = levelToken;
    std::string code;
    const std::size_t dash = levelToken.find('-');
    if (dash != std::string::npos) {
        level = levelToken.substr(0, dash);
        code = levelToken.substr(dash + 1);
    }
    if (!MapSeverity(level, out.severity)) return false;
    std::string rest = line.substr(colon + 1);
    rest = Trim(rest);
    // rest: "file:line[:col]: message" 或直接 message。
    const std::size_t messageColon = rest.find(": ");
    if (messageColon != std::string::npos) {
        const std::string location = rest.substr(0, messageColon);
        out.summary = Trim(rest.substr(messageColon + 2));
        const std::size_t last1 = location.find(':');
        const std::size_t last2 = location.rfind(':');
        out.sourceFile = location.substr(0, last1);
        if (last2 != last1 && last2 != std::string::npos) {
            out.sourceLine = location.substr(last1 + 1, last2 - last1 - 1);
            out.sourceColumn = location.substr(last2 + 1);
        } else if (last1 != std::string::npos) {
            out.sourceLine = location.substr(last1 + 1);
        }
    } else {
        out.summary = rest;
    }
    out.rawCode = code;
    out.model = "verilator";
    out.stage = "sim";
    return true;
}

// Yosys 行：top.v:12: ERROR: message   /   top.v:3:5: warning: message
bool ParseYosysLine(const std::string& line, DiagnosticParser::Parsed& out) {
    const std::size_t firstColon = line.find(':');
    if (firstColon == std::string::npos) return false;
    const std::size_t secondColon = line.find(':', firstColon + 1);
    if (secondColon == std::string::npos) return false;
    // 后置 severity 令牌。
    std::size_t levelStart = secondColon + 1;
    while (levelStart < line.size() && (line[levelStart] == ' ' || line[levelStart] == '\t')) {
        ++levelStart;
    }
    std::size_t levelEnd = levelStart;
    while (levelEnd < line.size() && line[levelEnd] != ':' && line[levelEnd] != ' ') {
        ++levelEnd;
    }
    if (levelEnd == levelStart) return false;
    const std::string levelToken = line.substr(levelStart, levelEnd - levelStart);
    std::string severity;
    if (!MapSeverity(levelToken, severity)) return false;
    std::size_t messageStart = levelEnd;
    while (messageStart < line.size() && (line[messageStart] == ':' || line[messageStart] == ' ')) {
        ++messageStart;
    }
    const std::string file = line.substr(0, firstColon);
    const std::string row = line.substr(firstColon + 1, secondColon - firstColon - 1);
    if (file.empty() || row.empty()) return false;
    // 行号必须是纯数字。
    for (char c : row) {
        if (!std::isdigit(static_cast<unsigned char>(c))) return false;
    }
    out.sourceFile = file;
    out.sourceLine = row;
    out.severity = severity;
    out.summary = Trim(line.substr(messageStart));
    out.model = "yosys";
    out.stage = "synth";
    return true;
}

} // namespace

namespace {

// 行号/列号：数字优先（schema location 为对象；非 tick/uid 不用字符串转数字）。
Json LineNumberOrNull(const std::string& text) {
    if (text.empty()) return Json(nullptr);
    try {
        const unsigned long long value = std::stoull(text);
        return Json(static_cast<std::uint64_t>(value));
    } catch (const std::exception&) {
        return Json(nullptr);  // 解析失败不伪造行号。
    }
}

} // namespace

std::vector<DiagnosticParser::Parsed> DiagnosticParser::Parse(const std::string& text) {
    std::vector<Parsed> out;
    std::istringstream stream(text);
    std::string line;
    while (std::getline(stream, line)) {
        const std::string trimmed = Trim(line);
        if (trimmed.empty()) continue;
        Parsed parsed;
        if (ParseVerilatorLine(trimmed, parsed)) {
            out.push_back(std::move(parsed));
            continue;
        }
        if (ParseYosysLine(trimmed, parsed)) {
            out.push_back(std::move(parsed));
        }
        // 其他行：跳过，不猜测根因。
    }
    return out;
}

Json ParsedDiagnosticsToJson(const std::vector<DiagnosticParser::Parsed>& parsed,
                             const std::string& defaultStage) {
    Json out = Json::array();
    std::size_t sequence = 0;
    for (const auto& item : parsed) {
        Json diagnostic;
        diagnostic["id"] = "diag-parsed-" + std::to_string(++sequence);
        diagnostic["code"] = item.rawCode.empty() ? "unclassified" : item.rawCode;
        diagnostic["severity"] = item.severity;
        diagnostic["stage"] = item.stage.empty() ? defaultStage : item.stage;
        diagnostic["summary"] = item.summary;
        diagnostic["origin"] = "tool";
        diagnostic["confidence_kind"] = "tool";
        diagnostic["raw_code"] = item.rawCode.empty() ? nullptr : Json(item.rawCode);
        diagnostic["location"] = Json{{"file", item.sourceFile.empty() ? nullptr
                                                                  : Json(item.sourceFile)},
                                      {"line", LineNumberOrNull(item.sourceLine)},
                                      {"column", LineNumberOrNull(item.sourceColumn)}};
        out.push_back(std::move(diagnostic));
    }
    return out;
}

} // namespace agent
} // namespace eda
