// SF-06：Job 报告归一化单元测试（edu.jobreport.v1）。
#include "eda-agent-gateway/ReportNormalizer.h"

#include <iostream>
#include <string>

using eda::agent::NormalizeJobReport;
using eda::agent::ReportNormalizeInput;

namespace {

int g_failures = 0;

void Check(bool ok, const char* msg) {
    if (ok) {
        std::cout << "  ok: " << msg << "\n";
    } else {
        ++g_failures;
        std::cout << "  FAIL: " << msg << "\n";
    }
}

eda::JobReport MakeReport(eda::JobState state) {
    eda::JobReport report;
    report.jobId = "job-1";
    report.jobType = "synth";
    report.pluginId = "eda-synth-yosys";
    report.state = state;
    report.exitCode = 0;
    return report;
}

} // namespace

int main() {
    // 9 态映射。
    {
        ReportNormalizeInput input;
        input.report = MakeReport(eda::JobState::TimedOut);
        const eda::Json data = NormalizeJobReport(input);
        Check(data["state"] == "TimedOut", "TimedOut state mapped (not Unknown)");
        input.report = MakeReport(eda::JobState::Cancelled);
        Check(NormalizeJobReport(input)["state"] == "Cancelled", "Cancelled mapped");
        input.report = MakeReport(eda::JobState::ValidatingArtifact);
        Check(NormalizeJobReport(input)["state"] == "Running",
              "internal ValidatingArtifact is normalized to public Running");
        Check(std::string(eda::agent::JobStateName(eda::JobState::Queued)) == "Queued",
              "JobStateName helper");
    }

    // 诊断：数组（含 severity/ir_coordinate）。
    {
        ReportNormalizeInput input;
        input.report = MakeReport(eda::JobState::Succeeded);
        input.report.diagnostics = eda::Json::array(
            {eda::Json{{"code", "EDU_LATCH_INFERRED"}, {"severity", "warning"},
                       {"stage", "synth"}, {"summary", "latch on q"},
                       {"ir_coordinate", "src/top.v:12"}}});
        const eda::Json data = NormalizeJobReport(input);
        Check(data["completeness"] == "complete", "array diagnostics -> complete");
        Check(data["diagnostics"].is_array() && data["diagnostics"].size() == 1,
              "array diagnostics normalized");
        const eda::Json diag = data["diagnostics"][0];
        Check(diag["severity"] == "warning", "diagnostic severity kept");
        Check(diag["location"].is_object() &&
                  diag["location"]["ir_coordinate"] == "src/top.v:12",
              "ir_coordinate becomes location");
        Check(diag["origin"] == "core", "diagnostic origin default core");
    }

    // 诊断：core 对象形态 {error: "..."}。
    {
        ReportNormalizeInput input;
        input.report = MakeReport(eda::JobState::Failed);
        input.report.diagnostics = eda::Json{{"error", "yosys exited 1"}};
        const eda::Json data = NormalizeJobReport(input);
        Check(data["completeness"] == "complete", "object diagnostics -> complete");
        Check(data["diagnostics"].is_array() && data["diagnostics"].size() == 1,
              "object diagnostics expanded");
        Check(data["diagnostics"][0]["raw_code"] == "error", "object key becomes raw_code");
        Check(data["diagnostics"][0]["summary"] == "yosys exited 1", "object value becomes summary");
    }

    // 诊断未提供（null）→ unavailable + reason。
    {
        ReportNormalizeInput input;
        input.report = MakeReport(eda::JobState::Succeeded);
        input.report.diagnostics = eda::Json(nullptr);
        const eda::Json data = NormalizeJobReport(input);
        Check(data["completeness"] == "unavailable", "null diagnostics -> unavailable");
        Check(data["diagnostics"].is_array() && data["diagnostics"].empty(),
              "unavailable diagnostics is empty array");
        Check(data.contains("reason") && data["reason"].is_string(), "unavailable carries reason");
    }

    // 空数组 = 完整扫描且未发现（completeness=complete）。
    {
        ReportNormalizeInput input;
        input.report = MakeReport(eda::JobState::Succeeded);
        input.report.diagnostics = eda::Json::array();
        const eda::Json data = NormalizeJobReport(input);
        Check(data["completeness"] == "complete",
              "empty array = scanned clean (distinct from unavailable)");
    }

    // metrics：裸值包装 + null → unavailable + reason；缺失不填 0。
    {
        ReportNormalizeInput input;
        input.report = MakeReport(eda::JobState::Succeeded);
        input.report.metrics = eda::Json{{"lut", 96},
                                         {"fmax_mhz", nullptr},
                                         {"cells", eda::Json{{"value", 120}, {"unit", "cells"}}}};
        const eda::Json data = NormalizeJobReport(input);
        Check(data["metrics"]["lut"]["value"] == 96 &&
                  data["metrics"]["lut"]["availability"] == "available",
              "bare metric wrapped as available");
        Check(data["metrics"]["fmax_mhz"]["availability"] == "unavailable" &&
                  data["metrics"]["fmax_mhz"].contains("reason"),
              "null metric -> unavailable + reason (not 0)");
        Check(data["metrics"]["cells"].contains("availability"),
              "object metric gains availability");
    }

    // artifacts：sha256/role/schema 保留。
    {
        ReportNormalizeInput input;
        input.report = MakeReport(eda::JobState::Succeeded);
        eda::Artifact artifact;
        artifact.id = "netlist";
        artifact.path = "artifacts/top.json";
        artifact.schema = "eda.netlist.yosys-json.v1";
        artifact.sha256 = "abc123";
        artifact.role = "primary";
        input.report.artifacts.push_back(artifact);
        const eda::Json data = NormalizeJobReport(input);
        Check(data["artifacts"].size() == 1, "artifact normalized");
        Check(data["artifacts"][0]["artifact_id"] == "netlist" &&
                  data["artifacts"][0]["sha256"] == "abc123",
              "artifact id/sha256 kept");
    }

    // 诊断：core 对象 {error:"多行工具日志"} → 升格解析为真实诊断。
    {
        ReportNormalizeInput input;
        input.report = MakeReport(eda::JobState::Failed);
        input.report.diagnostics = eda::Json{
            {"error", "top.v:12: ERROR: syntax error, unexpected IDENT\n"
                      "top.v:30: warning: latch inferred for `q'\n"
                      "%Warning-WIDTH: sim_tb.v:41: Bits of signal truncated\n"
                      "    some unrelated frame line\n"
                      "%Error-EXITNOTOK: sim_tb.v:9: simulation failed"}};
        const eda::Json data = NormalizeJobReport(input);
        Check(data["completeness"] == "complete", "parsed log diagnostics -> complete");
        Check(data["diagnostics"].is_array() && data["diagnostics"].size() == 4,
              "log lines parsed into individual diagnostics");
        const eda::Json first = data["diagnostics"][0];
        Check(first["severity"] == "error" && first["origin"] == "core",
              "yosys error line is error/core-origin (contract enum)");
        Check(first["location"]["file"] == "top.v" && first["location"]["line"] == 12,
              "yosys location preserved");
        Check(data["diagnostics"][1]["severity"] == "warning", "yosys warning mapped");
        Check(data["diagnostics"][2]["raw_code"] == "WIDTH" &&
                  data["diagnostics"][2]["location"]["file"] == "sim_tb.v" &&
                  data["diagnostics"][2]["location"]["line"] == 41,
              "verilator %Warning-CODE parsed with raw code and location");
        Check(data["diagnostics"][3]["raw_code"] == "EXITNOTOK" &&
                  data["diagnostics"][3]["severity"] == "error",
              "verilator %Error-CODE parsed");
    }

    // 诊断：core 对象 {timeout:true} → 不可解析仍保留框架条目。
    {
        ReportNormalizeInput input;
        input.report = MakeReport(eda::JobState::TimedOut);
        input.report.diagnostics = eda::Json{{"error", "process timed out"}, {"timeout", true}};
        const eda::Json data = NormalizeJobReport(input);
        Check(data["diagnostics"].size() == 2, "unparseable object keeps framework entries");
        Check(data["diagnostics"][0]["raw_code"] == "error" &&
                  data["diagnostics"][0]["severity"] == "error",
              "framework error entry raw_code");
        Check(data["diagnostics"][1]["raw_code"] == "timeout", "timeout entry preserved");
    }

    // legacy_unverified：老产物无指纹/版本。
    {
        ReportNormalizeInput input;
        input.report = MakeReport(eda::JobState::Succeeded);
        input.rawReportSchema = "main/jobs 1.0";
        input.legacyUnverified = true;
        const eda::Json data = NormalizeJobReport(input);
        Check(data["origin"] == "legacy", "legacy origin");
        Check(data["completeness"] == "legacy_unverified", "legacy completeness");
        Check(data["revision"].is_null() && data["input_fingerprint"].is_null(),
              "legacy has null revision/fingerprint");
        Check(data["raw_report_schema"] == "main/jobs 1.0", "raw schema preserved");
    }

    // SF-06：旧 reports（main/jobs/schemas/job-report.schema.json）errors[] → 规范 Diagnostic[]。
    // 逐字段核对映射表：code/severity/stage/summary/ir_coordinate/log_line 一项都不许丢。
    {
        ReportNormalizeInput input;
        input.report = MakeReport(eda::JobState::Failed);
        input.report.exitCode = 1;
        input.rawReportSchema = "job-report-1.0";
        input.legacyUnverified = true;
        input.report.diagnostics = eda::Json::array({
            eda::Json{{"code", "LATCH"}, {"severity", "warning"}, {"stage", "synthesis"},
                      {"ir_coordinate", "$flatten\\u$5.q"}, {"log_line", 42},
                      {"summary", "Latch inferred for signal 'q'."}},
            eda::Json{{"code", "SYNTAX"}, {"severity", "error"}, {"stage", "synthesis"},
                      {"log_line", 12}, {"summary", "syntax error near 'endmodule'."}},
        });
        const eda::Json data = NormalizeJobReport(input);
        Check(data["origin"] == "legacy" && data["raw_report_schema"] == "job-report-1.0",
              "legacy report keeps its origin and raw schema");
        Check(data["diagnostics"].is_array() && data["diagnostics"].size() == 2,
              "legacy errors[] become Diagnostic[]");
        const eda::Json first = data["diagnostics"][0];
        Check(first["raw_code"] == "LATCH" && first["code"] == "LATCH",
              "legacy error code kept in raw_code and code");
        Check(first["severity"] == "warning" && first["stage"] == "synthesis",
              "legacy severity/stage mapped without reinterpretation");
        Check(first["summary"] == "Latch inferred for signal 'q'.",
              "legacy summary carried over verbatim");
        // 用 value(...,默认) 逐项读取：字段缺失时得到默认值而不是抛异常，
        // 这样回归会表现为清晰的 FAIL，而不是在库断言处崩溃。
        const eda::Json firstLocation =
            first.contains("location") && first["location"].is_object() ? first["location"]
                                                                       : eda::Json::object();
        Check(eda::Json(firstLocation).value("ir_coordinate", std::string()) ==
                      "$flatten\\u$5.q" &&
                  firstLocation.value("log_line", 0) == 42,
              "legacy ir_coordinate and log_line both preserved in location");
        Check(first["confidence_kind"] == "tool" && first["origin"] == "core",
              "legacy entry normalized to a contract-valid diagnostic");
        const eda::Json secondLocation =
            data["diagnostics"][1].contains("location") &&
                    data["diagnostics"][1]["location"].is_object()
                ? data["diagnostics"][1]["location"]
                : eda::Json::object();
        Check(secondLocation.value("log_line", 0) == 12,
              "per-diagnostic log line kept for each entry");
        Check(data["completeness"] == "legacy_unverified",
              "legacy report stays legacy_unverified even with diagnostics");
    }

    // SF-06：未知原始码保留 raw_code 且不假定教学根因（code=unclassified 语义）。
    {
        ReportNormalizeInput input;
        input.report = MakeReport(eda::JobState::Failed);
        input.report.diagnostics = eda::Json::array({
            eda::Json{{"severity", "error"}, {"stage", "synth"},
                      {"summary", "tool said something we do not classify"}},
        });
        const eda::Json data = NormalizeJobReport(input);
        const eda::Json entry = data["diagnostics"][0];
        Check(entry["code"] == "UNCLASSIFIED",
              "code-less diagnostic is UNCLASSIFIED, not a guessed teaching root cause");
        Check(entry["raw_code"].is_null(),
              "missing original code stays null instead of being invented");
        Check(entry["summary"] == "tool said something we do not classify",
              "unknown diagnostic keeps the tool's own wording");
    }

    // SF-06：无定位信息时 location 必须为 null，不得伪造行号。
    {
        ReportNormalizeInput input;
        input.report = MakeReport(eda::JobState::Failed);
        input.report.diagnostics = eda::Json::array({
            eda::Json{{"code", "GENERIC"}, {"severity", "error"}, {"stage", "synth"},
                      {"summary", "no location available"}},
        });
        const eda::Json data = NormalizeJobReport(input);
        Check(data["diagnostics"][0]["location"].is_null(),
              "diagnostic without location reports null (no fabricated line numbers)");
    }

    // 边界：报告不得把本机绝对路径交给 Agent（metrics 结构化值 + location.file）。
    {
        ReportNormalizeInput input;
        input.report = MakeReport(eda::JobState::Failed);
        input.report.metrics = eda::Json{
            {"preflight",
             eda::Json{{"value",
                        eda::Json{{"g++", "E:\\download\\mingw64\\bin\\g++.exe"},
                                  {"verilator", "/opt/verilator/bin/verilator_bin"},
                                  {"note", "plain text is left untouched"}}},
                       {"unit", nullptr},
                       {"source", nullptr}}},
            {"lut", 96},
        };
        input.report.diagnostics = eda::Json::array({
            eda::Json{{"code", "GENERIC"}, {"severity", "error"}, {"stage", "synth"},
                      {"summary", "tool said something"}, {"file", "C:/proj/rtl/top.v"}},
        });
        const eda::Json data = NormalizeJobReport(input);
        const eda::Json preflight = data["metrics"]["preflight"]["value"];
        Check(preflight["g++"] == "g++.exe",
              "windows absolute tool path in a metric is reduced to its file name");
        Check(preflight["verilator"] == "verilator_bin",
              "posix absolute tool path in a metric is reduced to its file name");
        Check(preflight["note"] == "plain text is left untouched",
              "non-path metric strings are not rewritten");
        Check(data["metrics"]["preflight"].value("path_redacted", false),
              "redaction is explicit (path_redacted), never silent");
        Check(data["metrics"]["lut"].value("value", 0) == 96 &&
                  !data["metrics"]["lut"].contains("path_redacted"),
              "numeric metrics are untouched and not marked as redacted");
        const eda::Json location = data["diagnostics"][0]["location"];
        Check(location.value("file", std::string()) == "top.v" &&
                  location.value("file_redacted", false),
              "absolute diagnostic file path is reduced to its file name and marked");
        Check(data.dump().find("E:\\\\download") == std::string::npos &&
                  data.dump().find("/opt/verilator") == std::string::npos &&
                  data.dump().find("C:/proj") == std::string::npos,
              "no absolute local path survives anywhere in the report");
    }

    // 边界：工具**自由文本**里的本机路径同样不得交给 Agent。
    {
        ReportNormalizeInput input;
        input.report = MakeReport(eda::JobState::Failed);
        input.report.diagnostics = eda::Json::array({
            eda::Json{{"code", "SYNTH_FAIL"}, {"severity", "error"}, {"stage", "synth"},
                      {"summary", "Reading E:\\proj\\rtl\\top.v failed; see /opt/yosys/bin/yosys "
                                  "and /api/v1/jobs for details"}},
        });
        const eda::Json data = NormalizeJobReport(input);
        const eda::Json diagnostic = data["diagnostics"][0];
        const std::string summary = diagnostic.value("summary", std::string());
        Check(diagnostic.value("summary_paths_redacted", false),
              "tool free-text path redaction is marked, not silent");
        Check(summary.find("top.v") != std::string::npos &&
                  summary.find("E:\\proj") == std::string::npos,
              "windows path inside a tool summary is reduced to its file name");
        Check(summary.find("yosys") != std::string::npos &&
                  summary.find("/opt/yosys") == std::string::npos,
              "posix tool path inside a summary is reduced to its file name");
        Check(summary.find("/api/v1/jobs") != std::string::npos,
              "url-like paths are not mistaken for local paths");
    }
    {
        // 旧格式的纯字符串诊断同样裁剪。
        ReportNormalizeInput input;
        input.report = MakeReport(eda::JobState::Failed);
        input.report.diagnostics = eda::Json::array(
            {eda::Json("failed to open C:\\proj\\rtl\\top.v")});
        const eda::Json data = NormalizeJobReport(input);
        const eda::Json diagnostic = data["diagnostics"][0];
        Check(diagnostic.value("summary_paths_redacted", false) &&
                  diagnostic.value("summary", std::string()).find("C:\\proj") ==
                      std::string::npos,
              "legacy string diagnostic is path-redacted as well");
    }

    std::cout << (g_failures == 0 ? "ALL PASS" : "FAILURES") << "\n";
    return g_failures == 0 ? 0 : 1;
}
