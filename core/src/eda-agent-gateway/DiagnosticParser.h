#pragma once

#include <eda/api/Types.h>

#include <string>
#include <vector>

namespace eda {
namespace agent {

// SF-06：从工具输出日志文本解析规范 Diagnostic[]（wx 无关）。
// 支持两类行格式：
//   Yosys      — "<file>:<line>[:<col>]: error:|warning: <message>"
//   Verilator  — "%Error[-CODE]|%Warning[-CODE]|%Info: <file>:<line>: <message>"
// 无法解析的行跳过（不猜根因）；解析失败 never fabricates locations。
class DiagnosticParser {
public:
    struct Parsed {
        std::string severity;    // error | warning | info
        std::string stage;       // 由来源前缀推导（synth/sim），可空
        std::string summary;
        std::string sourceFile;
        std::string sourceLine;
        std::string sourceColumn;
        std::string rawCode;     // verilator 的 -CODE；yosys 为空
        std::string model;       // yosys | verilator
    };

    // 返回解析到的条目数；unclassified/severity 白名单以 schema 为准（info/warning/error）。
    static std::vector<Parsed> Parse(const std::string& text);
};

// 把解析结果装配为规范 Diagnostic 对象数组（origin=tool，confidence_kind=tool，
// 未知 rawCode 保留 + unclassified）。
Json ParsedDiagnosticsToJson(const std::vector<DiagnosticParser::Parsed>& parsed,
                             const std::string& defaultStage);

} // namespace agent
} // namespace eda
