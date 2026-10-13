// SF-01：edu-agent v1 契约 fixtures 校验（不依赖 HTTP 库；仅 nlohmann + 内置子集 schema 校验）。
// 目的：为 SigFlow Gateway / UCAgent 提供可机器校验的字段真源基线。
#include <eda/api/Types.h>
#include <eda/api/schemas.hpp>

#include "eda-core/SchemaRegistry.h"
#include "eda-core/CoreSchemas.h"

#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

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

bool LoadJson(const std::string& path, eda::Json& out) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return false;
    const std::string content((std::istreambuf_iterator<char>(input)),
                              std::istreambuf_iterator<char>());
    try {
        out = eda::Json::parse(content);
    } catch (const std::exception&) {
        return false;
    }
    return true;
}

// 读取 edu-agent 契约 schema 的必需字段集合（子集校验：type/required）。
bool RegisterSchemaFile(eda::SimpleSchemaRegistry& registry, const std::string& schemaPath,
                        const std::string& id) {
    eda::Json schema;
    if (!LoadJson(schemaPath, schema)) return false;
    return registry.RegisterSchema(id, schema);
}

// $ref 解析：把 "design-node.schema.json" / "schemas/source-ref.schema.json" 这类
// 文件引用按文件名映射到 <契约目录>/schemas/<name>。同文档内部指针由注册表自己解析。
eda::SimpleSchemaRegistry::RefResolver MakeFileResolver(const std::string& contractDir) {
    return [contractDir](const std::string& reference, eda::Json& out) {
        const std::size_t hash = reference.find('#');
        const std::string filePart =
            hash == std::string::npos ? reference : reference.substr(0, hash);
        if (filePart.empty()) return false;
        const std::size_t slash = filePart.find_last_of("/\\");
        const std::string name = slash == std::string::npos ? filePart : filePart.substr(slash + 1);
        return LoadJson(contractDir + "/schemas/" + name, out) && out.is_object();
    };
}

} // namespace

int main(int argc, char** argv) {
    const std::string dir = argc > 1 ? argv[1] : "";

    eda::SimpleSchemaRegistry registry;
    // 严格契约校验：解析 $ref（含文件引用与内部指针），无法解析的引用视为失败。
    registry.SetRefResolver(MakeFileResolver(dir));
    registry.SetStrictRefs(true);
    Check(RegisterSchemaFile(registry, dir + "/schemas/capabilities.schema.json",
                             "capabilities.json"),
          "register capabilities schema");
    Check(RegisterSchemaFile(registry, dir + "/schemas/diagnostic.schema.json",
                             "diagnostic.json"),
          "register diagnostic schema");
    Check(RegisterSchemaFile(registry, dir + "/schemas/job-report-view.schema.json",
                             "job-report-view.json"),
          "register job-report schema");
    Check(RegisterSchemaFile(registry, dir + "/schemas/project-context.schema.json",
                             "project-context.json"),
          "register project-context schema");
    Check(RegisterSchemaFile(registry, dir + "/schemas/snapshot.schema.json",
                             "snapshot.json"),
          "register snapshot schema");
    Check(RegisterSchemaFile(registry, dir + "/schemas/event.schema.json", "event.json"),
          "register event schema");
    Check(RegisterSchemaFile(registry, dir + "/schemas/project-state.schema.json",
                             "project-state.json"),
          "register project-state schema");
    Check(RegisterSchemaFile(registry, dir + "/schemas/grant.schema.json", "grant.json"),
          "register grant schema");
    Check(RegisterSchemaFile(registry, dir + "/schemas/ui-receipt.schema.json",
                             "ui-receipt.json"),
          "register ui-receipt schema");
    Check(RegisterSchemaFile(registry, dir + "/schemas/wave-signals.schema.json",
                             "wave-signals.json"),
          "register wave-signals schema");
    Check(RegisterSchemaFile(registry, dir + "/schemas/wave-query.schema.json",
                             "wave-query.json"),
          "register wave-query schema");
    Check(RegisterSchemaFile(registry, dir + "/schemas/envelope.schema.json",
                             "envelope.json"),
          "register envelope schema");
    // NG-05 / NG-09：局部设计证据、上下文查询与旧历史只读桥。
    Check(RegisterSchemaFile(registry, dir + "/schemas/design-node.schema.json",
                             "design-node.json"),
          "register design-node schema");
    Check(RegisterSchemaFile(registry, dir + "/schemas/context-query.schema.json",
                             "context-query.json"),
          "register context-query schema");
    Check(RegisterSchemaFile(registry, dir + "/schemas/legacy-job.schema.json",
                             "legacy-job.json"),
          "register legacy-job schema");
    // NG-07：自研 Agent 服务侧冻结的卡片/会话/事件 DTO。
    Check(RegisterSchemaFile(registry, dir + "/schemas/agent-health.schema.json",
                             "agent-health.json"),
          "register agent-health schema");
    Check(RegisterSchemaFile(registry, dir + "/schemas/agent-capabilities.schema.json",
                             "agent-capabilities.json"),
          "register agent-capabilities schema");
    Check(RegisterSchemaFile(registry, dir + "/schemas/teaching-card.schema.json",
                             "teaching-card.json"),
          "register teaching-card schema");
    Check(RegisterSchemaFile(registry, dir + "/schemas/plan-card.schema.json",
                             "plan-card.json"),
          "register plan-card schema");
    Check(RegisterSchemaFile(registry, dir + "/schemas/agent-session.schema.json",
                             "agent-session.json"),
          "register agent-session schema");
    Check(RegisterSchemaFile(registry, dir + "/schemas/agent-run.schema.json",
                             "agent-run.json"),
          "register agent-run schema");
    Check(RegisterSchemaFile(registry, dir + "/schemas/agent-event.schema.json",
                             "agent-event.json"),
          "register agent-event schema");

    eda::Json golden;
    Check(RegisterSchemaFile(registry, dir + "/schemas/teaching-action.schema.json", "teaching-action.json"),
          "register teaching-action schema");
    Check(RegisterSchemaFile(registry, dir + "/schemas/teaching-action-result.schema.json", "teaching-action-result.json"),
          "register teaching-action-result schema");
    Check(LoadJson(dir + "/fixtures/golden.json", golden) && golden.is_array(),
          "load golden fixtures");
    if (!golden.is_array()) {
        std::cout << "FAILURES\n";
        return 1;
    }

    // 按名字取 fixture，避免用例与数组下标耦合（新增 fixture 不应改坏既有断言）。
    const auto fixtureNamed = [&golden](const std::string& name) -> eda::Json {
        for (const auto& fixture : golden) {
            if (fixture.value("name", std::string()) == name) return fixture["document"];
        }
        return eda::Json();
    };
    const eda::Json coreReport = fixtureNamed("job-report-view.core");
    const eda::Json savedContext = fixtureNamed("project-context.saved");
    const eda::Json activeGrant = fixtureNamed("grant.active");
    const eda::Json activeReceipt = fixtureNamed("ui-receipt.active");
    const eda::Json waveSignals = fixtureNamed("wave-signals.page");
    const eda::Json waveQuery = fixtureNamed("wave-query.exact");
    Check(coreReport.is_object() && savedContext.is_object() && activeGrant.is_object() &&
              activeReceipt.is_object() && waveSignals.is_object() && waveQuery.is_object(),
          "named fixtures are present for negative/positive cases");

    for (const auto& fixture : golden) {
        const std::string name = fixture.value("name", std::string("?"));
        std::string schemaFile = fixture.value("schema", std::string());
        // fixtures 用文件名（如 diagnostic.schema.json）；注册用短 id（diagnostic.json）。
        const std::string suffix = ".schema.json";
        std::string schemaId = schemaFile;
        if (schemaId.size() > suffix.size() &&
            schemaId.compare(schemaId.size() - suffix.size(), suffix.size(), suffix) == 0) {
            schemaId = schemaId.substr(0, schemaId.size() - suffix.size()) + ".json";
        }
        std::string error;
        const bool ok = registry.Validate(schemaId, fixture["document"], error);
        Check(ok, (name + " validates against " + schemaFile).c_str());
        if (!ok) {
            std::cout << "      reason: " << error << "\n";
        }
    }

    // 负例：缺必填字段必须被拒。
    {
        std::string error;
        eda::Json missing = eda::Json::object();
        missing["id"] = "diag-x";  // 缺 code/severity/...
        Check(!registry.Validate("diagnostic.json", missing, error),
              "diagnostic missing required fields rejected");
    }
    {
        std::string error;
        eda::Json badOrigin = coreReport;
        badOrigin["origin"] = "made-up";  // 枚举不在子集校验范围，改验缺字段
        badOrigin.erase("state");
        Check(!registry.Validate("job-report-view.json", badOrigin, error),
              "job report missing 'state' rejected");
    }

    // 负例：未知枚举值必须显式失败（spec §5.1：未知枚举不静默接受）。
    {
        std::string error;
        eda::Json unknownEnum = coreReport;
        Check(registry.Validate("job-report-view.json", unknownEnum, error),
              "known job report enum baseline validates");
        unknownEnum["origin"] = "made-up";
        Check(!registry.Validate("job-report-view.json", unknownEnum, error),
              "unknown 'origin' enum value rejected");
        unknownEnum = coreReport;
        unknownEnum["state"] = "AlmostDone";
        Check(!registry.Validate("job-report-view.json", unknownEnum, error),
              "unknown job state rejected");
    }
    {
        std::string error;
        eda::Json unknownEvent = eda::Json::object();
        unknownEvent["event_id"] = "ev-1";
        unknownEvent["sequence"] = "1";
        unknownEvent["project_id"] = "project-0123456789abcdef01234567";
        unknownEvent["type"] = "job/exploded";  // 不在事件类型枚举内
        unknownEvent["timestamp"] = "2026-10-01T00:00:00Z";
        unknownEvent["data"] = eda::Json::object();
        Check(!registry.Validate("event.json", unknownEvent, error),
              "unknown event type rejected");
        unknownEvent["type"] = "job/finished";
        Check(registry.Validate("event.json", unknownEvent, error),
              "known event type accepted");
    }
    {
        std::string error;
        eda::Json badReceipt = activeReceipt;
        badReceipt["level"] = "answer";  // L4 之外的自造等级
        Check(!registry.Validate("ui-receipt.json", badReceipt, error),
              "unknown receipt level rejected");
        badReceipt = activeReceipt;
        badReceipt["state_version"] = 7;  // 64 位计数必须是十进制字符串
        Check(!registry.Validate("ui-receipt.json", badReceipt, error),
              "numeric state_version rejected (decimal string required)");
    }
    {
        std::string error;
        eda::Json badQuery = waveQuery;
        badQuery["completeness"] = "probably-exact";
        Check(!registry.Validate("wave-query.json", badQuery, error),
              "unknown wave completeness rejected");
        badQuery = waveQuery;
        badQuery["start_tick"] = 0;  // 64 位 tick 必须是十进制字符串
        Check(!registry.Validate("wave-query.json", badQuery, error),
              "numeric tick rejected (decimal string required)");
    }

    // 正例：未知可选字段必须被忽略（向前兼容），空数组合法，超限/中文路径可表达。
    {
        std::string error;
        eda::Json withExtra = activeGrant;  // grant.active
        withExtra["future_optional_field"] = "ignored";
        const bool extraOk = registry.Validate("grant.json", withExtra, error);
        Check(extraOk, "unknown optional field is ignored (forward compatible)");
        if (!extraOk) std::cout << "      reason: " << error << "\n";

        eda::Json emptyCollections = activeGrant;
        emptyCollections["steps"] = eda::Json::array();
        const bool emptyOk = registry.Validate("grant.json", emptyCollections, error);
        Check(emptyOk, "empty collection is valid");
        if (!emptyOk) std::cout << "      reason: " << error << "\n";
    }
    {
        std::string error;
        // 中文与空格路径：相对 POSIX 路径是契约允许的表示。
        eda::Json unicode = savedContext;  // project-context.saved
        unicode["sources"][0]["path"] = "rtl/\u6d4b\u8bd5 \u6a21\u5757/top.v";
        const bool unicodeOk = registry.Validate("project-context.json", unicode, error);
        Check(unicodeOk, "non-ASCII path with spaces is a valid relative path");
        if (!unicodeOk) std::cout << "      reason: " << error << "\n";
    }
    {
        std::string error;
        // 64 位边界：tick/uid 用十进制字符串承载，不得溢出或被截断。
        eda::Json boundary = waveSignals;  // wave-signals.page
        boundary["time_range"]["end_tick"] = "18446744073709551615";
        const bool boundaryOk = registry.Validate("wave-signals.json", boundary, error);
        Check(boundaryOk, "64-bit boundary tick as decimal string is valid");
        if (!boundaryOk) std::cout << "      reason: " << error << "\n";
        boundary["time_range"]["end_tick"] = "18446744073709551616";  // 2^64
        const bool patternOk = registry.Validate("wave-signals.json", boundary, error);
        Check(patternOk, "pattern check accepts any decimal string (range enforced by service)");
        if (!patternOk) std::cout << "      reason: " << error << "\n";
    }
    {
        std::string error;
        // 失败信封的错误码枚举必须与实现一致。
        eda::Json failure = eda::Json::object();
        failure["schema_version"] = "edu.api.v1";
        failure["request_id"] = "req-1";
        failure["trace_id"] = "trace-1";
        failure["error"] = eda::Json{{"code", "RESOURCE_EXHAUSTED"},
                                     {"message", "response exceeds limit"},
                                     {"retryable", true}};
        Check(registry.Validate("envelope.json#/$defs/failure", failure, error) ||
                  registry.Validate("envelope.json", failure, error),
              "failure envelope with known code is accepted");
    }

    {
        std::string error;
        // 严格 $ref 校验必须真的生效：只破坏**嵌套**（被 $ref 指向的）字段也要被拒。
        eda::Json nested = fixtureNamed("context-query.selection-buffer");
        Check(nested.is_object() && nested["design_nodes"].is_array(),
              "nested-ref fixture is available");
        const bool baselineOk = registry.Validate("context-query.json", nested, error);
        Check(baselineOk, "context-query fixture with nested $ref validates (baseline)");
        if (!baselineOk) std::cout << "      reason: " << error << "\n";

        eda::Json badOrigin = nested;
        badOrigin["design_nodes"][0]["node"]["origin"] = "made-up";  // design-node 枚举
        const bool originRejected = !registry.Validate("context-query.json", badOrigin, error);
        Check(originRejected,
              "nested $ref target enum is enforced (design_nodes[].node.origin)");
        if (!originRejected) std::cout << "      reason: " << error << "\n";

        eda::Json badBuffer = nested;
        badBuffer["buffer_excerpts"][0]["usable_for_execution"] = true;  // 契约恒为 false
        const bool bufferRejected = !registry.Validate("context-query.json", badBuffer, error);
        Check(bufferRejected, "nested const is enforced (buffer usable_for_execution=false)");
        if (!bufferRejected) std::cout << "      reason: " << error << "\n";

        eda::Json badDiagnostic = coreReport;
        badDiagnostic["diagnostics"] = eda::Json::array({eda::Json{{"id", "diag-1"}}});
        const bool diagnosticRejected =
            !registry.Validate("job-report-view.json", badDiagnostic, error);
        Check(diagnosticRejected,
              "nested $ref target required fields are enforced (diagnostics[] vs diagnostic)");
        if (!diagnosticRejected) std::cout << "      reason: " << error << "\n";
    }

    std::cout << (g_failures == 0 ? "ALL PASS" : "FAILURES") << "\n";
    return g_failures == 0 ? 0 : 1;
}
