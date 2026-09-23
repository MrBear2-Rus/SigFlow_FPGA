// P2 DoD 前置：双向同步（解析 → SFTree）回归样例（非 GUI）。
// 用桩 wxEvtHandler 驱动 SigFlowTree，验证 parse→tree 结构与 ToVerilog 往返、uid 稳定性。
#include "SigTree.h"

#include <tree_sitter/api.h>
#include <wx/event.h>

#include <cstdint>
#include <iostream>
#include <string>
#include <tuple>
#include <unordered_map>

extern "C" TSLanguage* tree_sitter_verilog();

// 事件定义（正常由 MainFrame.cpp 提供；本测试不链接 MainFrame，故在此定义）。
wxDEFINE_EVENT(EVT_SIGFLOWNODE_ADD, wxCommandEvent);
wxDEFINE_EVENT(EVT_SIGFLOWNODE_DEL, wxCommandEvent);
wxDEFINE_EVENT(EVT_SIGFLOWNODE_CHANGED, wxCommandEvent);

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

SigTreeNode* FindDescendantOfType(SigTreeNode* node, SigTreeNodeType type) {
    if (node == nullptr) return nullptr;
    for (SigTreeNode* child : node->GetChildren()) {
        if (child == nullptr) continue;
        if (child->type == type) return child;
        if (SigTreeNode* found = FindDescendantOfType(child, type)) return found;
    }
    return nullptr;
}

SigTreeNode* FindByName(SigTreeNode* node, const std::string& name) {
    if (node == nullptr) return nullptr;
    if (node->GetName() == name) return node;
    for (SigTreeNode* child : node->GetChildren()) {
        if (SigTreeNode* found = FindByName(child, name)) return found;
    }
    return nullptr;
}

int CountDescendantsOfType(SigTreeNode* node, SigTreeNodeType type) {
    if (node == nullptr) return 0;
    int count = 0;
    for (SigTreeNode* child : node->GetChildren()) {
        if (child == nullptr) continue;
        if (child->type == type) ++count;
        count += CountDescendantsOfType(child, type);
    }
    return count;
}

std::unordered_map<std::uint64_t, std::tuple<int, int>> ParseInto(
    SigFlowTree& tree, const std::string& code, const std::string& filePath) {
    TSParser* parser = ts_parser_new();
    ts_parser_set_language(parser, tree_sitter_verilog());
    TSTree* tsTree =
        ts_parser_parse_string(parser, nullptr, code.c_str(), static_cast<uint32_t>(code.size()));
    TSNode rootNode = ts_tree_root_node(tsTree);
    TSTreeCursor cursor = ts_tree_cursor_new(rootNode);
    std::unordered_map<std::uint64_t, std::tuple<int, int>> outMap;
    std::string mutablePath = filePath;
    std::string mutableCode = code;
    tree.UpdateTreeFromTS(&cursor, tree.root, mutablePath, mutableCode, outMap);
    ts_tree_cursor_delete(&cursor);
    ts_tree_delete(tsTree);
    ts_parser_delete(parser);
    tree.ReindexUids();
    return outMap;
}

const char* kCode =
    "module top(input a, input b, output y);\n"
    "  assign y = a & b;\n"
    "endmodule\n";

} // namespace

int main() {
    wxEvtHandler handler;  // 桩事件处理器（SigFlowTree 不再依赖 MainFrame）
    SigFlowTree tree(&handler);
    tree.LoadProject("testproj");
    Check(tree.root != nullptr, "ProjectNode created");
    Check(tree.root->type == SigTreeNodeType::Project, "root is Project");

    const auto outMap = ParseInto(tree, kCode, "top.v");
    Check(!outMap.empty(), "outMap populated");

    SigTreeNode* fileNode = FindDescendantOfType(tree.root, SigTreeNodeType::File);
    Check(fileNode != nullptr, "FileNode created");
    SigTreeNode* topNode = FindDescendantOfType(tree.root, SigTreeNodeType::Top);
    Check(topNode != nullptr, "TopNode created");
    Check(topNode != nullptr && topNode->GetName() == "Module top",
          "top module name is 'Module top'");

    Check(CountDescendantsOfType(tree.root, SigTreeNodeType::Top) == 1, "exactly one TopNode");

    // uid 稳定性 + outMap 以 uid 为键
    Check(fileNode != nullptr && fileNode->uid != 0, "FileNode has uid");
    Check(topNode != nullptr && topNode->uid != 0, "TopNode has uid");
    Check(fileNode != nullptr && outMap.count(fileNode->uid) == 1, "outMap keyed by FileNode uid");
    Check(topNode != nullptr && outMap.count(topNode->uid) == 1, "outMap keyed by TopNode uid");
    Check(fileNode != nullptr && topNode != nullptr && fileNode->uid != topNode->uid,
          "uids are unique");
    Check(fileNode != nullptr && tree.NodeByUid(fileNode->uid) == fileNode,
          "NodeByUid resolves FileNode");
    Check(topNode != nullptr && tree.NodeByUid(topNode->uid) == topNode,
          "NodeByUid resolves TopNode");

    // ToVerilog 往返
    const std::string verilog = topNode != nullptr ? topNode->ToVerilog() : std::string();
    Check(verilog.find("module top") != std::string::npos, "ToVerilog contains 'module top'");
    Check(verilog.find("endmodule") != std::string::npos, "ToVerilog contains 'endmodule'");

    // 二次解析：uid 稳定（同节点 uid 不变）
    const std::uint64_t fileUidBefore = fileNode != nullptr ? fileNode->uid : 0;
    SigTreeNode* topBefore = topNode;
    ParseInto(tree, kCode, "top.v");
    SigTreeNode* fileNode2 = FindDescendantOfType(tree.root, SigTreeNodeType::File);
    SigTreeNode* topNode2 = FindDescendantOfType(tree.root, SigTreeNodeType::Top);
    Check(fileNode2 != nullptr && fileNode2->uid == fileUidBefore, "FileNode uid stable on re-parse");
    Check(topNode2 == topBefore, "TopNode pointer stable on re-parse");

    tree.ClearTree();
    Check(tree.root == nullptr, "ClearTree resets root");

    // 编辑往返：树结构变更 → ToVerilog 反映变更（编辑一致性回归）
    {
        SigFlowTree editTree(&handler);
        editTree.LoadProject("edit");
        ParseInto(editTree, kCode, "top.v");
        SigTreeNode* top = FindDescendantOfType(editTree.root, SigTreeNodeType::Top);
        Check(top != nullptr, "edit: top found");
        if (top != nullptr) {
            TopNode* topModule = static_cast<TopNode*>(top);
            const std::size_t baselineSignals = topModule->signals.size();

            // 新增 wire 信号（经树 API，等价于画布/编辑器写入路径之一）
            SignalNode* wire = editTree.AddSignal(topModule,
                                                  editTree.arena.make<SignalNode>("w", SignalType::Wire));
            Check(wire != nullptr, "edit: AddSignal returns node");
            Check(topModule->signals.size() == baselineSignals + 1,
                  "edit: signal list grew after add");
            Check(wire != nullptr && wire->GetName() == "wire w", "edit: added signal name");

            // 删除该信号
            editTree.RemoveSignal(topModule, wire);
            Check(topModule->signals.size() == baselineSignals,
                  "edit: signal list shrinks after remove");
            bool removed = true;
            for (const auto* signal : topModule->signals) {
                if (signal == wire) removed = false;
            }
            Check(removed, "edit: removed signal no longer in list");
        }
        editTree.ClearTree();
    }

    std::cout << (g_failures == 0 ? "ALL PASS" : "FAILURES") << "\n";
    return g_failures == 0 ? 0 : 1;
}
