// NG-05 / SF-07：局部设计证据与未保存缓冲边界冒烟测试（真实 loopback HTTP）。
//
// 覆盖：
//   * 节点映射的 available / ambiguous / stale / unavailable 四种语义；
//   * 同 revision 可回到源码、源码变化后降级、显式旧 revision 返回 409；
//   * 未保存缓冲只用于解释：usable_for_execution 恒为 false，且不出现在设计节点/源码证据里；
//   * max_bytes 预算与 omitted/reason 显式化；路径越界不返回内容。
#include "eda-agent-gateway/GatewayServer.h"

#include <eda/api/Types.h>

#include <httplib.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

int g_failures = 0;

void Check(bool ok, const char* message) {
    if (ok) {
        std::cout << "  ok: " << message << "\n";
    } else {
        ++g_failures;
        std::cout << "  FAIL: " << message << "\n";
    }
}

eda::Json GetJson(int port, const std::string& path, const std::string& token, int& status) {
    httplib::Client client("127.0.0.1", port);
    client.set_connection_timeout(2, 0);
    client.set_read_timeout(5, 0);
    httplib::Headers headers;
    if (!token.empty()) headers.emplace("Authorization", "Bearer " + token);
    const auto res = client.Get(path.c_str(), headers);
    if (!res) {
        status = -1;
        return eda::Json::object();
    }
    status = res->status;
    try {
        return eda::Json::parse(res->body);
    } catch (const std::exception&) {
        return eda::Json::object();
    }
}

eda::Json PostJson(int port, const std::string& path, const std::string& token,
                   const eda::Json& body, int& status) {
    httplib::Client client("127.0.0.1", port);
    client.set_connection_timeout(2, 0);
    client.set_read_timeout(5, 0);
    httplib::Headers headers;
    if (!token.empty()) headers.emplace("Authorization", "Bearer " + token);
    const auto res = client.Post(path.c_str(), headers, body.dump(), "application/json");
    if (!res) {
        status = -1;
        return eda::Json::object();
    }
    status = res->status;
    try {
        return eda::Json::parse(res->body);
    } catch (const std::exception&) {
        return eda::Json::object();
    }
}

eda::agent::DesignSourceRef MakeRef(const std::string& sourceId, const std::string& path,
                                    const std::string& hash, std::uint64_t startLine,
                                    std::uint64_t endLine) {
    eda::agent::DesignSourceRef ref;
    ref.sourceId = sourceId;
    ref.path = path;
    ref.fileHash = hash;
    ref.startLine = startLine;
    ref.endLine = endLine;
    return ref;
}

eda::agent::DesignNodeRecord MakeNode(const std::string& nodeId, const std::string& kind) {
    eda::agent::DesignNodeRecord node;
    node.nodeId = nodeId;
    node.kind = kind;
    node.name = nodeId;
    node.moduleName = "top";
    node.instancePath = "top." + nodeId;
    node.revision = "";
    return node;
}

} // namespace

int main() {
    namespace fs = std::filesystem;
    const fs::path projectRoot =
        fs::temp_directory_path() /
        ("sigflow_design_context_" + std::to_string(
             std::chrono::high_resolution_clock::now().time_since_epoch().count()));
    std::error_code cleanupError;
    fs::remove_all(projectRoot, cleanupError);
    fs::create_directories(projectRoot / "rtl");
    const std::string rtlText = "module top(input d, en, output reg q);\n"
                                "  always @(*) if (en) q = d;\n"
                                "endmodule\n";
    std::ofstream(projectRoot / "rtl" / "top.v", std::ios::binary | std::ios::trunc) << rtlText;
    std::ofstream(projectRoot / "sigflow.project", std::ios::binary | std::ios::trunc)
        << R"JSON({
  "build": {"top_module": ["top"]},
  "paths": {"source_files": ["rtl/top.v"]},
  "fpga": {"target_profile": "test-target", "yosys_strategy": "baseline"}
})JSON";

    eda::agent::GatewayConfig config;
    config.instanceId = "inst-design-context";
    config.edition = "edu";
    config.token = "agent-token";
    config.uiToken = "ui-token";
    config.port = 0;

    const auto provider = []() {
        std::vector<eda::agent::ReadyPlugin> ready;
        ready.push_back({"eda-synth-yosys", "1.0.0", {"synth"}});
        return ready;
    };

    eda::agent::GatewayServer server(config, provider);
    std::string error;
    std::string projectId;
    Check(server.RefreshProjectSnapshotStateFromDisk(projectRoot, false, true, projectId, error),
          "gateway builds project context for the design-context project");
    Check(!projectId.empty(), "project id is assigned");

    if (!server.Start(error)) {
        std::cout << "start error: " << error << "\n";
        std::cout << "FAILURES\n";
        return 1;
    }
    server.RunAsync();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const int port = server.Port();
    const std::string base = "/api/v1/projects/" + projectId;

    int status = 0;
    const eda::Json context = GetJson(port, base + "/context", "agent-token", status);
    Check(status == 200, "GET /context returns 200");
    const std::string revision = context["data"].value("revision", std::string());
    Check(!revision.empty(), "context carries a revision");

    std::string sourceId;
    std::string fileHash;
    for (const auto& source : context["data"]["sources"]) {
        if (source.value("path", std::string()) == "rtl/top.v") {
            sourceId = source.value("source_id", std::string());
            fileHash = source.value("sha256", std::string());
        }
    }
    Check(!sourceId.empty() && !fileHash.empty(), "context exposes the registered source id/hash");

    // ---- 节点表（同一 revision） ----
    std::vector<eda::agent::DesignNodeRecord> nodes;
    {
        eda::agent::DesignNodeRecord node = MakeNode("node-top", "module");
        node.sourceRefs.push_back(MakeRef(sourceId, "rtl/top.v", fileHash, 1, 3));
        eda::agent::DesignPort port;
        port.name = "q";
        port.direction = "output";
        port.width = 1;
        node.ports.push_back(port);
        nodes.push_back(node);
    }
    {
        eda::agent::DesignNodeRecord node = MakeNode("node-amb", "instance");
        node.declaredStatus = eda::agent::MappingStatus::Ambiguous;
        node.candidates = {"node-top", "node-other"};
        node.sourceRefs.push_back(MakeRef(sourceId, "rtl/top.v", fileHash, 2, 2));
        nodes.push_back(node);
    }
    {
        eda::agent::DesignNodeRecord node = MakeNode("node-synth", "net");
        node.declaredStatus = eda::agent::MappingStatus::Unavailable;
        node.declaredReason = "synthesized_alias";
        nodes.push_back(node);
    }
    nodes.push_back(MakeNode("node-bare", "instance"));
    {
        eda::agent::DesignNodeRecord node = MakeNode("node-buffer", "net");
        node.unsavedOnly = true;
        nodes.push_back(node);
    }
    {
        eda::agent::DesignNodeRecord node = MakeNode("node-changed", "instance");
        node.sourceRefs.push_back(MakeRef(sourceId, "rtl/top.v",
                                          std::string(64, 'a'), 1, 3));
        nodes.push_back(node);
    }
    {
        eda::agent::DesignNodeRecord node = MakeNode("node-escape", "instance");
        node.sourceRefs.push_back(MakeRef("source-000000000000000000000000",
                                          "../sigflow.project", std::string(64, 'b'), 1, 2));
        nodes.push_back(node);
    }
    {
        eda::agent::DesignNodeRecord node = MakeNode("node-missing-file", "instance");
        node.sourceRefs.push_back(MakeRef("source-111111111111111111111111",
                                          "rtl/not-there.v", std::string(64, 'c'), 1, 2));
        nodes.push_back(node);
    }
    server.SetDesignNodes(projectId, revision, nodes);

    // ---- 四种映射语义 ----
    {
        const eda::Json body = GetJson(port, base + "/design/nodes/node-top", "agent-token", status);
        Check(status == 200, "GET /design/nodes/{node} returns 200");
        Check(body["data"].value("mapping_status", std::string()) == "available",
              "mapped node reports mapping_status=available");
        Check(body["data"].value("reason", std::string()) == "available", "available node reason");
        Check(body["data"]["node"]["source_refs"][0].value("path", std::string()) == "rtl/top.v",
              "available node returns the project-relative source ref");
        Check(body["data"].value("usable_for_execution", true) == false,
              "design context is never an executable input");
        Check(body["data"]["node"]["ports"][0].value("name", std::string()) == "q",
              "node ports are carried through");
        Check(body["data"].value("revision", std::string()) == revision,
              "node answer is bound to the current revision");
    }
    {
        const eda::Json body = GetJson(port, base + "/design/nodes/node-amb", "agent-token", status);
        Check(status == 200, "ambiguous node returns 200");
        Check(body["data"].value("mapping_status", std::string()) == "ambiguous",
              "declared ambiguity is reported as ambiguous");
        Check(body["data"]["candidates"].size() == 2,
              "ambiguous node lists the candidate node ids without guessing one");
    }
    {
        const eda::Json body =
            GetJson(port, base + "/design/nodes/node-synth", "agent-token", status);
        Check(body["data"].value("mapping_status", std::string()) == "unavailable",
              "declared unavailable node stays unavailable");
        Check(body["data"].value("reason", std::string()) == "synthesized_alias",
              "unavailable reason is preserved verbatim");
    }
    {
        const eda::Json body =
            GetJson(port, base + "/design/nodes/node-bare", "agent-token", status);
        Check(body["data"].value("mapping_status", std::string()) == "unavailable" &&
                  body["data"].value("reason", std::string()) == "no_source_mapping",
              "node without a reliable source mapping is unavailable");
    }
    {
        const eda::Json body =
            GetJson(port, base + "/design/nodes/node-buffer", "agent-token", status);
        Check(body["data"].value("mapping_status", std::string()) == "available" &&
                  body["data"].value("unsaved_only", false) &&
                  body["data"].value("reason", std::string()) == "unsaved_only",
              "buffer-only node is available for explanation and flagged unsaved_only");
        Check(body["data"]["node"].value("origin", std::string()) == "unsaved_buffer",
              "buffer-only node is labelled as unsaved_buffer origin");
    }
    {
        const eda::Json body =
            GetJson(port, base + "/design/nodes/node-changed", "agent-token", status);
        Check(body["data"].value("mapping_status", std::string()) == "stale" &&
                  body["data"].value("reason", std::string()) == "source_changed",
              "source hash mismatch degrades the mapping to stale");
    }
    {
        const eda::Json body =
            GetJson(port, base + "/design/nodes/node-escape", "agent-token", status);
        Check(body["data"].value("mapping_status", std::string()) == "stale" &&
                  body["data"].value("reason", std::string()) == "unmapped_path",
              "path escaping the project never becomes a usable mapping");
        Check(body["data"].dump().find("top_module") == std::string::npos,
              "escaped path content is not surfaced");
    }
    {
        const eda::Json body =
            GetJson(port, base + "/design/nodes/node-missing-file", "agent-token", status);
        Check(body["data"].value("mapping_status", std::string()) == "stale" &&
                  body["data"].value("reason", std::string()) == "source_missing",
              "missing source file degrades the mapping to stale");
    }
    {
        const eda::Json body =
            GetJson(port, base + "/design/nodes/node-unknown", "agent-token", status);
        Check(status == 200 &&
                  body["data"].value("mapping_status", std::string()) == "unavailable" &&
                  body["data"].value("reason", std::string()) == "node_not_registered",
              "unknown node id returns unavailable instead of a guessed mapping");
    }

    // ---- 身份 / 版本 / 未知工程 ----
    {
        GetJson(port, base + "/design/nodes/node-top", "", status);
        Check(status == 401, "design node query without token is rejected");
        GetJson(port, "/api/v1/projects/project-000000000000000000000000/design/nodes/x",
                "agent-token", status);
        Check(status == 404, "unknown project returns 404");
        GetJson(port, base + "/design/nodes/node-top?revision=rev-1-000000000000", "agent-token",
                status);
        Check(status == 409, "explicit stale revision returns 409 STALE_REVISION");
        const eda::Json body = GetJson(port, base + "/design/nodes/node-top?revision=" + revision,
                                       "agent-token", status);
        Check(status == 200, "explicit current revision succeeds");
        Check(body["data"].value("mapping_status", std::string()) == "available",
              "explicit current revision keeps the mapping available");
    }

    // ---- revision 竞态：旧映射不得冒充新版本 ----
    {
        std::vector<eda::agent::DesignNodeRecord> oldNodes;
        eda::agent::DesignNodeRecord node = MakeNode("node-top", "module");
        node.sourceRefs.push_back(MakeRef(sourceId, "rtl/top.v", fileHash, 1, 3));
        oldNodes.push_back(node);
        server.SetDesignNodes(projectId, "rev-999-000000000000", oldNodes);
        const eda::Json body =
            GetJson(port, base + "/design/nodes/node-top", "agent-token", status);
        Check(body["data"].value("mapping_status", std::string()) == "stale" &&
                  body["data"].value("reason", std::string()) == "revision_changed",
              "mapping captured at another revision is stale, not available");
        server.SetDesignNodes(projectId, revision, nodes);
    }
    // ---- 源码在同 revision 内被外部改写：降级为 stale，不串版本 ----
    {
        std::ofstream(projectRoot / "rtl" / "top.v", std::ios::binary | std::ios::trunc)
            << rtlText << "// external edit\n";
        const eda::Json body =
            GetJson(port, base + "/design/nodes/node-top", "agent-token", status);
        Check(body["data"].value("mapping_status", std::string()) == "stale" &&
                  body["data"].value("reason", std::string()) == "source_changed",
              "source rewritten under the same revision is detected as stale");
        std::ofstream(projectRoot / "rtl" / "top.v", std::ios::binary | std::ios::trunc) << rtlText;
        const eda::Json restored =
            GetJson(port, base + "/design/nodes/node-top", "agent-token", status);
        Check(restored["data"].value("mapping_status", std::string()) == "available",
              "restoring the exact bytes restores the mapping");
    }

    // ---- 选择上下文与未保存缓冲 ----
    const std::string bufferMarker = "BUFFER_ONLY_MARKER_9f3c";
    {
        eda::agent::SelectionContext selection;
        selection.revision = revision;
        selection.selectedNodeIds = {"node-top", "node-amb"};
        selection.dirty = true;
        selection.bufferHash = "buffer-hash-1";
        selection.stateVersion = 7;
        eda::agent::BufferExcerpt excerpt;
        excerpt.path = "rtl/top.v";
        excerpt.bufferHash = "buffer-hash-1";
        excerpt.startLine = 1;
        excerpt.endLine = 3;
        excerpt.text = bufferMarker + std::string(80 * 1024, 'x');
        selection.bufferExcerpts.push_back(excerpt);
        server.UpdateSelectionContext(projectId, selection);
    }
    {
        const eda::Json request{{"revision", revision},
                                {"max_bytes", 4 * 1024 * 1024},
                                {"needs", {"selection", "design", "buffer"}}};
        const eda::Json body = PostJson(port, base + "/context/query", "agent-token", request, status);
        Check(status == 200, "context/query with selection/design/buffer returns 200");
        Check(body["data"]["selection"].value("status", std::string()) == "ok",
              "selection resolves at the current revision");
        Check(body["data"]["selection"].value("usable_for_execution", true) == false,
              "selection is explicitly not an executable input");
        Check(body["data"]["selection"]["selected_node_ids"].size() == 2,
              "selection carries the selected node ids");
        Check(body["data"]["design_nodes"].size() == 2,
              "design need resolves the selected nodes");
        Check(body["data"]["design_nodes"][0].value("mapping_status", std::string()) == "available" &&
                  body["data"]["design_nodes"][1].value("mapping_status", std::string()) ==
                      "ambiguous",
              "each selected node keeps its own mapping status");
        Check(body["data"]["buffer_excerpts"].size() == 1,
              "unsaved buffer excerpt is returned for explanation");
        const eda::Json& buffer = body["data"]["buffer_excerpts"][0];
        Check(buffer.value("origin", std::string()) == "unsaved_buffer",
              "buffer excerpt is labelled unsaved_buffer");
        Check(buffer.value("usable_for_execution", true) == false,
              "buffer excerpt is never usable for execution");
        Check(!buffer.contains("source_id"),
              "buffer excerpt carries no source id and cannot become a snapshot input");
        Check(buffer.value("truncated", false),
              "oversized buffer excerpt is truncated instead of unbounded");
        Check(buffer.value("text", std::string()).size() <= 64 * 1024,
              "buffer excerpt honours the byte cap");
        Check(buffer.value("text", std::string()).rfind(bufferMarker, 0) == 0,
              "buffer excerpt keeps the leading content");
        Check(body.dump().find(projectRoot.string()) == std::string::npos,
              "no absolute local path is exposed in the context package");
        Check(body["data"]["used_bytes"].get<std::size_t>() <=
                  body["data"]["max_bytes"].get<std::size_t>(),
              "used_bytes stays within max_bytes");
        Check(body["data"]["sources"].empty(),
              "unrequested sources are not silently added");
    }
    {
        // 设计需求单独指定 node_id，并受 max_bytes 预算约束。
        eda::Json need = eda::Json::object();
        need["kind"] = "design";
        need["node_id"] = "node-top";
        eda::Json request = eda::Json::object();
        request["revision"] = revision;
        request["max_bytes"] = 1;
        request["needs"] = eda::Json::array({need});
        const eda::Json body = PostJson(port, base + "/context/query", "agent-token", request, status);
        Check(status == 200, "context/query with a tiny budget returns 200");
        Check(body["data"]["design_nodes"].empty(), "tiny budget yields no design node payload");
        bool budgetOmitted = false;
        for (const auto& item : body["data"]["omitted"]) {
            if (item.value("kind", std::string()) == "design" &&
                item.value("reason", std::string()) == "exceeds max_bytes budget") {
                budgetOmitted = true;
            }
        }
        Check(budgetOmitted, "dropped design payload is reported in omitted with a reason");
    }
    {
        eda::agent::SelectionContext selection;
        selection.revision = revision;
        selection.selectedNodeIds = {"node-top"};
        selection.dirty = false;
        server.UpdateSelectionContext(projectId, selection);
        const eda::Json request{{"revision", revision},
                                {"needs", {"buffer", "diagnostics"}}};
        const eda::Json body = PostJson(port, base + "/context/query", "agent-token", request, status);
        Check(body["data"]["buffer_excerpts"].empty(),
              "no buffer excerpt is served when the project is not dirty");
        bool bufferOmitted = false;
        bool diagnosticsOmitted = false;
        for (const auto& item : body["data"]["omitted"]) {
            if (item.value("kind", std::string()) == "buffer" &&
                item.value("reason", std::string()) == "no_unsaved_buffer") {
                bufferOmitted = true;
            }
            if (item.value("kind", std::string()) == "diagnostics") {
                diagnosticsOmitted = true;
            }
        }
        Check(bufferOmitted, "buffer need is explicitly omitted when there is no unsaved buffer");
        Check(diagnosticsOmitted, "unsupported need is explicitly omitted with a reason");
    }
    {
        const eda::Json request{{"revision", "rev-1-000000000000"}, {"needs", {"selection"}}};
        PostJson(port, base + "/context/query", "agent-token", request, status);
        Check(status == 409, "context/query with a stale revision returns 409");
    }
    {
        // 选择上下文未登记时不得凭空出现节点。
        server.ClearDesignContext(projectId);
        GetJson(port, base + "/design/nodes/node-top", "agent-token", status);
        Check(status == 200, "design node query on a cleared project still answers");
        const eda::Json request{{"revision", revision}, {"needs", {"selection"}}};
        const eda::Json body = PostJson(port, base + "/context/query", "agent-token", request, status);
        bool cleared = false;
        for (const auto& item : body["data"]["omitted"]) {
            if (item.value("kind", std::string()) == "selection") cleared = true;
        }
        Check(cleared, "cleared design context reports selection as omitted");
        Check(body["data"]["design_nodes"].empty(), "no design nodes are invented after clearing");
    }

    server.Stop();
    fs::remove_all(projectRoot, cleanupError);
    std::cout << (g_failures == 0 ? "ALL PASSED\n" : "FAILURES\n");
    return g_failures == 0 ? 0 : 1;
}
