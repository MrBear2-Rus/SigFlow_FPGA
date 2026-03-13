#include "SigTree.h"
#include "MainFrame.h"

#include <json/json.h>
#include <iostream>
#include <fstream>
#include <filesystem>
#include <memory>
#include <windows.h>
#include <regex>
#include <set>
#include <queue>

#include <unordered_set>

extern "C" TSLanguage* tree_sitter_verilog();

namespace fs = std::filesystem;

TopNodeType isTSNodeTop(std::string node_type);
SecondNodeType isTSNodeSecond(std::string node_type);
bool isTSNodeNet(std::string node_type);
void CollectSigTreeNodeInfoTS(SigTreeNode* node, TSNode& TSnode, std::string filepath, std::string code);

SigFlowTree::SigFlowTree(MainFrame* parent) : m_parent(parent) {
    const char* top_temp = R"(
                ;; 1. 模块定义捕获（独立，保证只要有模块名就能匹配）
                (module_declaration
                  (module_header 
                    (simple_identifier) @mod.name))

                ;; 2. 端口捕获（利用层级无关性，直接抓取 ansi_port_declaration）
                (ansi_port_declaration
                  (net_port_header1
                    [(port_direction) @port.dir])
                  (port_identifier
                    (simple_identifier) @port.name))

        )";
    top = ts_query_new(tree_sitter_verilog(), top_temp, strlen(top_temp), &top_error_offset, &top_error_type);

    const char* sec_temp = R"(
                 (module_instantiation
                    (simple_identifier)@modinst.modname
                    (hierarchical_instance
                        (name_of_instance)@modinst.instname
                        (list_of_port_connections
                            (named_port_connection
                                (port_identifier
                                    (simple_identifier)@modinst.portname)
                                (expression
                                    (primary
                                        (simple_identifier)@modinst.portconn)))))
                    )
                 (gate_instantiation
                    (n_input_gatetype)@gate.type
                    (n_input_gate_instance
                        (name_of_instance)@gate.name
                        (output_terminal)@gate.output
                        (input_terminal)@gate.input)
                    )

                (continuous_assign
                  (list_of_net_assignments
                    (net_assignment
                      (net_lvalue) @assign.lhs
                      (expression) @assign.rhs
                    )
                  )
                )

(always_construct
  (statement
    (statement_item
      (procedural_timing_control_statement
        (event_control
          (event_expression
            (edge_identifier) @clk.edge
            (expression) @clk.name))
        
        (statement_or_null
          (_ 
            (_
              (_
                (statement_or_null
                  (_
                    (_
                      [
                        (nonblocking_assignment
                          (variable_lvalue) @assign.lhs
                          (delay_or_event_control)? @assign.delay
                          (expression) @assign.rhs)
                        (blocking_assignment
                          (variable_lvalue) @assign.lhs
                          (delay_or_event_control)? @assign.delay
                          (expression) @assign.rhs)
                      ]
                    )
                  )
                )
              )
            )
          )
        )
      )
    )
  )
)

        )";

    second = ts_query_new(tree_sitter_verilog(), sec_temp, strlen(sec_temp), &second_error_offset, &second_error_type);

    const char* net_temp = R"(
                    (net_declaration
                        (net_type) @net.type
                        (list_of_net_decl_assignments
                            (net_decl_assignment
                                (simple_identifier)@net.name)))
                    (data_declaration
                        (data_type_or_implicit1)@net.type
                        (list_of_variable_decl_assignments
                            (variable_decl_assignment)@net.name))

        )";

    net = ts_query_new(tree_sitter_verilog(), net_temp, strlen(net_temp), &net_error_offset, &net_error_type);
}

void SigFlowTree::LoadProject(std::string projectPath) {
    this->root = arena.make<ProjectNode>(projectPath);
}

struct ExpressionResult {
    std::string template_text;
    std::vector<Port> extracted_ports;
};

ExpressionResult FormalizeExpression(
    TSNode node,
    const std::string& code,
    std::unordered_map<std::string, std::string>& placeholderMap,  // 信号名 -> 占位符映射（表达式内去重）
    int exp_id,                                                     // 当前表达式序号
    int& localCounter                                               // 表达式内输入计数器（引用）
) {
    ExpressionResult result;
    const char* type = ts_node_type(node);

    // 1. 如果是变量标识符：将其替换为占位符，不进行去重
    if (strcmp(type, "simple_identifier") == 0) {
        std::string original_conn = code.substr(ts_node_start_byte(node),
            ts_node_end_byte(node) - ts_node_start_byte(node));

        std::string placeholder;
        auto it = placeholderMap.find(original_conn);
        if (it != placeholderMap.end()) {
            placeholder = it->second;                     // 复用占位符
        }
        else {
            if (exp_id != -1) {
                placeholder = "in" + std::to_string(exp_id) + "_" + std::to_string(localCounter++);
            }
            else {
                placeholder = "in" + std::to_string(localCounter++);
            }
            placeholderMap[original_conn] = placeholder;  // 记录映射

            // 创建端口（仅当第一次出现时）
            Port p;
            p.identifier = placeholder;
            p.conn = original_conn;
            p.direction = PortDirection::In;
            result.extracted_ports.push_back(p);
        }

        result.template_text = placeholder;
        return result;
    }

    // 2. 如果是叶子节点但不是变量（比如运算符 & | ^ , 常数 1'b0, 括号）
    uint32_t child_count = ts_node_child_count(node);
    if (child_count == 0) {
        result.template_text = code.substr(ts_node_start_byte(node),
            ts_node_end_byte(node) - ts_node_start_byte(node));
        return result;
    }

    // 3. 递归处理所有子节点，并保留子节点之间的所有字符（包括空格、换行、运算符）
    uint32_t last_pos = ts_node_start_byte(node);
    for (uint32_t i = 0; i < child_count; i++) {
        TSNode child = ts_node_child(node, i);

        // 捕获当前子节点之前的任何原始字符（比如括号或空格）
        result.template_text += code.substr(last_pos, ts_node_start_byte(child) - last_pos);

        // 递归处理子节点
        ExpressionResult child_res = FormalizeExpression(child, code, placeholderMap, exp_id, localCounter);

        // 合并结果
        result.template_text += child_res.template_text;
        result.extracted_ports.insert(result.extracted_ports.end(),
            child_res.extracted_ports.begin(),
            child_res.extracted_ports.end());

        last_pos = ts_node_end_byte(child);
    }

    // 补齐最后一个子节点到父节点末尾的字符
    if (last_pos < ts_node_end_byte(node)) {
        result.template_text += code.substr(last_pos, ts_node_end_byte(node) - last_pos);
    }

    return result;
}

void SigFlowTree::UpdateTreeFromTS(TSTreeCursor* cursor, SigTreeNode* SigRoot,std::string& filePath, std::string& code, std::unordered_map<SigTreeNode*, std::tuple<int, int>>& outMap) {
    TSNode currentNode = ts_tree_cursor_current_node(cursor);
    SigTreeNode* newParent = SigRoot;
    std::string node_type = ts_node_type(currentNode);

    if (ts_node_is_extra(currentNode)) {
        if (ts_tree_cursor_goto_first_child(cursor)) {
            // 递归子节点
            do {
                UpdateTreeFromTS(cursor, newParent, filePath, code, outMap);
            } while (ts_tree_cursor_goto_next_sibling(cursor));

            // 处理完所有子节点后，务必跳回父节点
            ts_tree_cursor_goto_parent(cursor);
        }
        return ;
    }
    // 首先判断parent类型
    if (newParent->type == SigTreeNodeType::Project) {
        // 找File
        std::string node_type = ts_node_type(currentNode);
        if (node_type == "source_file") {
            FileNode* fn = arena.make<FileNode>(filePath);
            fn = static_cast<FileNode*>(this->AddChild(newParent, fn));
            outMap[fn] = std::make_tuple(ts_node_start_point(currentNode).row, ts_node_end_point(currentNode).row);
            newParent = fn;
        }
    }
    else if (newParent->type == SigTreeNodeType::File) {
        // 找Top
        TopNodeType top_type = isTSNodeTop(node_type);
        if (top_type != TopNodeType::Null) {
            // 找Port和ID
            std::string id;
            std::vector<Port> in;
            std::vector<Port> out;

            TSQueryCursor* cursor = ts_query_cursor_new();
            ts_query_cursor_exec(cursor, top, currentNode);

            TSQueryMatch match;
            while (ts_query_cursor_next_match(cursor, &match)) {
                Port p;
                p.direction = PortDirection::InOut;

                for (uint16_t i = 0; i < match.capture_count; i++) {
                    TSQueryCapture capture = match.captures[i];

                    // 3. 识别零件的标签名（通过 ID 换取字符串）
                    uint32_t name_len;
                    const char* tag_name = ts_query_capture_name_for_id(top, capture.index, &name_len);

                    // 4. 根据标签名分发逻辑
                    if (strcmp(tag_name, "mod.name") == 0) {
                        TSNode nameNode = capture.node;
                        id = code.substr(ts_node_start_byte(nameNode),
                            ts_node_end_byte(nameNode) - ts_node_start_byte(nameNode));

                    }
                    else if (strcmp(tag_name, "port.dir") == 0) {
                        std::string d = code.substr(ts_node_start_byte(capture.node),
                            ts_node_end_byte(capture.node) - ts_node_start_byte(capture.node));
                        if (d == "input") p.direction = PortDirection::In;
                        else if (d == "output") p.direction = PortDirection::Out;
                    }
                    else if (strcmp(tag_name, "port.name") == 0) {
                        p.identifier = code.substr(ts_node_start_byte(capture.node),
                            ts_node_end_byte(capture.node) - ts_node_start_byte(capture.node));
                        if (p.direction == PortDirection::In) in.push_back(p);
                        else if (p.direction == PortDirection::Out) out.push_back(p);
                    }
                }
            }
            ts_query_cursor_delete(cursor);

            TopNode* tn = arena.make<TopNode>(id, top_type);
            tn->UpdateInPorts(in);
            tn->UpdateOutPorts(out);
            tn = static_cast<TopNode*>(this->AddChild(newParent, tn));
            outMap[tn] = std::make_tuple(ts_node_start_point(currentNode).row, ts_node_end_point(currentNode).row);
            newParent = tn;
        }
    }
    else if (newParent->type == SigTreeNodeType::Top) {
        TopNode* tParent = static_cast<TopNode*>(newParent);
        SecondNodeType second_type = isTSNodeSecond(node_type);
        // 找Net
        if (node_type == "net_declaration") {
            TSQueryCursor* cursor = ts_query_cursor_new();
            ts_query_cursor_exec(cursor, net, currentNode);

            TSQueryMatch match;
            std::string id;
            SignalType st = SignalType::Wire;
            SigTreeNode* netParent = newParent;
            while (ts_query_cursor_next_match(cursor, &match)) {

                for (uint16_t i = 0; i < match.capture_count; i++) {
                    TSQueryCapture capture = match.captures[i];

                    uint32_t name_len;
                    const char* tag_name = ts_query_capture_name_for_id(net, capture.index, &name_len);

                    if (strcmp(tag_name, "net.name") == 0) {
                        TSNode nameNode = capture.node;
                        id = code.substr(ts_node_start_byte(nameNode),
                            ts_node_end_byte(nameNode) - ts_node_start_byte(nameNode));
                        st = SignalType::Wire;
                    }
                }
                SignalNode tmpNode(id, st);
                SignalNode* nn = static_cast<SignalNode*>(this->AddChild(netParent, &tmpNode));
                
                if (nn) {
                    outMap[nn] = std::make_tuple(ts_node_start_point(currentNode).row, ts_node_end_point(currentNode).row);
                    SignalTable.emplace(nn->identifier, nn);
                    newParent = nn;
            }
            
        }
            ts_query_cursor_delete(cursor);
        }
        else if (node_type == "data_declaration") {
            TSQueryCursor* cursor = ts_query_cursor_new();
            ts_query_cursor_exec(cursor, net, currentNode);
            std::string id;
            TSQueryMatch match;
            SigTreeNode* dataParent = newParent;
            while (ts_query_cursor_next_match(cursor, &match)) {
                for (uint16_t i = 0; i < match.capture_count; i++) {
                    TSQueryCapture capture = match.captures[i];

                    uint32_t name_len;
                    const char* tag_name = ts_query_capture_name_for_id(net, capture.index, &name_len);

                    if (strcmp(tag_name, "net.name") == 0) {
                        TSNode nameNode = capture.node;
                        id = code.substr(ts_node_start_byte(nameNode),
                            ts_node_end_byte(nameNode) - ts_node_start_byte(nameNode));
                    }
                }
                SignalNode tmpNode(id, SignalType::Reg);
                SignalNode* nn = static_cast<SignalNode*>(AddChild(dataParent, &tmpNode));
                if (nn) {
                    outMap[nn] = std::make_tuple(ts_node_start_point(currentNode).row, ts_node_end_point(currentNode).row);
                SignalTable.emplace(nn->identifier, nn);
                newParent = nn;
            }
        }
            ts_query_cursor_delete(cursor);
        }
        // 找Second
        else if (second_type != SecondNodeType::Null) {
            if (second_type == SecondNodeType::ModuleInstance) {
                TSQueryCursor* cursor = ts_query_cursor_new();
                ts_query_cursor_exec(cursor, second, currentNode);

                ModuleInstNode* mn = arena.make<ModuleInstNode>();
                TSQueryMatch match;
                std::string id;
                std::string def;
                std::vector<Port> ps;
                while (ts_query_cursor_next_match(cursor, &match)) {
                    Port p;
                    p.direction = PortDirection::InOut;

                    for (uint16_t i = 0; i < match.capture_count; i++) {
                        TSQueryCapture capture = match.captures[i];

                        uint32_t name_len;
                        const char* tag_name = ts_query_capture_name_for_id(second, capture.index, &name_len);

                        if (strcmp(tag_name, "modinst.modname") == 0) {
                            TSNode nameNode = capture.node;
                            def = code.substr(ts_node_start_byte(nameNode),
                                ts_node_end_byte(nameNode) - ts_node_start_byte(nameNode));
                            mn->defIdentifier = def;
                        }
                        else if (strcmp(tag_name, "modinst.instname") == 0) {
                            TSNode nameNode = capture.node;
                            id = code.substr(ts_node_start_byte(nameNode),
                                ts_node_end_byte(nameNode) - ts_node_start_byte(nameNode));
                            mn->identifier = id;
                        }
                        else if (strcmp(tag_name, "modinst.portname") == 0) {
                            TSNode nameNode = capture.node;
                            p.identifier = code.substr(ts_node_start_byte(nameNode),
                                ts_node_end_byte(nameNode) - ts_node_start_byte(nameNode));
                        }
                        else if (strcmp(tag_name, "modinst.portconn") == 0) {
                            TSNode nameNode = capture.node;
                            p.conn = code.substr(ts_node_start_byte(nameNode),
                                ts_node_end_byte(nameNode) - ts_node_start_byte(nameNode));
                            mn->inout_ports.push_back(p);
                        }
                    }
                }
                mn = static_cast<ModuleInstNode*>(this->AddChild(newParent, mn));
                outMap[mn] = std::make_tuple(ts_node_start_point(currentNode).row, ts_node_end_point(currentNode).row);
                newParent = mn;
            }
            else if (second_type == SecondNodeType::GateInstance) {
                TSQueryCursor* cursor = ts_query_cursor_new();
                ts_query_cursor_exec(cursor, second, currentNode);

                TSQueryMatch match;
                uint32_t capture_index;

                uint32_t last_node_id = 0;
                const char* last_tag = "";

                std::string id;
                GateType gt;
                std::vector<std::string> in_conn;
                std::vector<std::string> out_conn;

                while (ts_query_cursor_next_capture(cursor, &match, &capture_index)) {
                    const TSQueryCapture& capture = match.captures[capture_index];
                    uint32_t name_len;
                    const char* tag_name = ts_query_capture_name_for_id(second, capture.index, &name_len);

                    uint32_t current_node_id = ts_node_start_byte(capture.node);
                    if (current_node_id == last_node_id && strcmp(tag_name, last_tag) == 0) {
                        continue; // 同样的标签在同样的位置，跳过！
                    }
                    last_node_id = current_node_id;
                    last_tag = tag_name;

                    std::string text = code.substr(ts_node_start_byte(capture.node),
                        ts_node_end_byte(capture.node) - ts_node_start_byte(capture.node));

                    if (strcmp(tag_name, "gate.type") == 0) {
                        gt = GateTypeFromString(text);
                    }
                    else if (strcmp(tag_name, "gate.name") == 0) {
                        id = text;
                    }
                    else if (strcmp(tag_name, "gate.output") == 0) {
                        out_conn.push_back(text);
                    }
                    else if (strcmp(tag_name, "gate.input") == 0) {
                        in_conn.push_back(text);
                    }
                }
                GateInstNode* gn = arena.make<GateInstNode>(id, gt);
                gn->in_ports[0].conn = in_conn[0];
                gn->in_ports[1].conn = in_conn[1];
                gn->out_ports[0].conn = out_conn[0];
                gn = static_cast<GateInstNode*>(this->AddChild(newParent, gn));
                outMap[gn] = std::make_tuple(ts_node_start_point(currentNode).row, ts_node_end_point(currentNode).row);
                newParent = gn;
            }
            else if (second_type == SecondNodeType::Always) {
                TSQueryCursor* cursor = ts_query_cursor_new();
                ts_query_cursor_exec(cursor, second, currentNode);

                TSQueryMatch match;
                uint32_t capture_index;

                uint32_t last_node_id = 0;
                const char* last_tag = "";

                EdgeType et = EdgeType::Posedge; // 默认
                std::string id;
                std::unordered_set<std::string> inPortsSet;
                std::unordered_set<std::string> outPortsSet;

                // 先创建 AlwaysNode，后续填充端口和语句
                AlwaysNode* an = arena.make<AlwaysNode>("", et); // 临时标识符，后面会设置
                std::unique_ptr<AlwaysStatement> currentStmt;

                int pendingExpId = -1;  // 暂存当前语句的表达式序号

                while (ts_query_cursor_next_capture(cursor, &match, &capture_index)) {
                    const TSQueryCapture& capture = match.captures[capture_index];
                    uint32_t name_len;
                    const char* tag_name = ts_query_capture_name_for_id(second, capture.index, &name_len);

                    uint32_t current_node_id = ts_node_start_byte(capture.node);
                    if (current_node_id == last_node_id && strcmp(tag_name, last_tag) == 0) {
                        continue;
                    }
                    last_node_id = current_node_id;
                    last_tag = tag_name;

                    std::string text = code.substr(ts_node_start_byte(capture.node),
                        ts_node_end_byte(capture.node) - ts_node_start_byte(capture.node));

                    if (strcmp(tag_name, "clk.edge") == 0) {
                        if (text == "posedge") et = EdgeType::Posedge;
                        else if (text == "negedge") et = EdgeType::Negedge;
                    }
                    else if (strcmp(tag_name, "clk.name") == 0) {
                        // 时钟输入端口统一命名为 "CLK"，原始信号名存入 conn
                        auto it = std::find_if(an->in_ports.begin(), an->in_ports.end(),
                            [](const Port& p) { return p.identifier == "CLK"; });
                        if (it == an->in_ports.end()) {
                            an->in_ports.push_back(Port("CLK", PortDirection::In, text));
                        }
                        else {
                            // 若已存在，可覆盖或忽略；这里简单忽略
                        }
                        id = "@" + text;       // 节点标识符仍保留原始时钟名，便于区分不同 always
                        an->identifier = id;
                    }
                    else if (strcmp(tag_name, "assign.lhs") == 0) {
                        if (currentStmt) an->addStatement(std::move(currentStmt));

                        pendingExpId = an->getStatementCount();          // 当前语句的索引即为表达式序号
                        std::string outName = "out" + std::to_string(pendingExpId);   // 抽象输出端口名
                        std::string lhsName = text;                      // 原始左值信号名

                        // 添加输出端口，identifier 为抽象名，conn 为原始信号名
                        if (std::find_if(an->out_ports.begin(), an->out_ports.end(),
                            [&outName](const Port& p) { return p.identifier == outName; }) == an->out_ports.end()) {
                            an->out_ports.push_back(Port(outName, PortDirection::Out, lhsName));
                        }

                        currentStmt = std::make_unique<AlwaysStatement>(false, "", 0.0f, outName, std::vector<std::string>{});
                    }
                    else if (strcmp(tag_name, "assign.rhs") == 0) {
                        if (!currentStmt || pendingExpId == -1) continue;

                        int exp_id = pendingExpId;
                        pendingExpId = -1;  // 消费后重置

                        std::unordered_map<std::string, std::string> localPlaceholderMap;
                        int localCounter = 1;
                        ExpressionResult res = FormalizeExpression(
                            capture.node, code, localPlaceholderMap, exp_id, localCounter
                        );

                        // 设置表达式模板
                        currentStmt->nb_or_b_expression = res.template_text;

                        // 处理提取的端口（每个表达式内唯一的信号）
                        for (const auto& p : res.extracted_ports) {
                            // 将占位符加入语句的输入列表（供后续替换用）
                            currentStmt->in_port_names.push_back(p.identifier);
                            // 直接追加到 AlwaysNode 的 in_ports（不同表达式允许同名信号重复）
                            an->in_ports.push_back(p);
                        }
                    }
                    else if (strcmp(tag_name, "assign.delay") == 0) {
                        if (!currentStmt) continue;
                        // 尝试解析延迟数值（可能为 #10 等形式）
                        // 这里简化处理：去掉 '#' 并转换为浮点数
                        std::string delayStr = text;
                        size_t pos = delayStr.find('#');
                        if (pos != std::string::npos) delayStr = delayStr.substr(pos + 1);
                        try {
                            currentStmt->delay = std::stof(delayStr);
                        }
                        catch (...) {
                            currentStmt->delay = 0.0f;
                        }
                    }
                }

                // 添加最后一个语句
                if (currentStmt) {
                    an->addStatement(std::move(currentStmt));
                }

                ts_query_cursor_delete(cursor);

                // 如果始终没有找到时钟名，使用默认标识符
                if (an->identifier.empty()) {
                    an->identifier = "@unknown_clk";
                }
                an->edgeType = et;

                an = static_cast<AlwaysNode*>(this->AddChild(newParent, an));
                outMap[an] = std::make_tuple(ts_node_start_point(currentNode).row, ts_node_end_point(currentNode).row);
                newParent = an;
            }
            else if (second_type == SecondNodeType::ContinuousAssign) {
                TSQueryCursor* cursor = ts_query_cursor_new();
                ts_query_cursor_exec(cursor, second, currentNode);

                TSQueryMatch match;
                uint32_t capture_index;

                uint32_t last_node_id = 0;
                const char* last_tag = "";

                int gate_input_count = 1;
                std::string id;
                std::string exp;
                std::vector<Port> in;
                Port out;

                while (ts_query_cursor_next_capture(cursor, &match, &capture_index)) {
                    const TSQueryCapture& capture = match.captures[capture_index];
                    uint32_t name_len;
                    const char* tag_name = ts_query_capture_name_for_id(second, capture.index, &name_len);

                    uint32_t current_node_id = ts_node_start_byte(capture.node);
                    if (current_node_id == last_node_id && strcmp(tag_name, last_tag) == 0) {
                        continue;
                    }
                    last_node_id = current_node_id;
                    last_tag = tag_name;

                    std::string text = code.substr(ts_node_start_byte(capture.node),
                        ts_node_end_byte(capture.node) - ts_node_start_byte(capture.node));

                    if (strcmp(tag_name, "assign.lhs") == 0) {
                        TSNode nameNode = capture.node;
                        id = code.substr(ts_node_start_byte(nameNode),
                            ts_node_end_byte(nameNode) - ts_node_start_byte(nameNode));
                        out = Port("out", PortDirection::Out, code.substr(ts_node_start_byte(nameNode),
                            ts_node_end_byte(nameNode) - ts_node_start_byte(nameNode)));
                    }
                    else if (strcmp(tag_name, "assign.rhs") == 0) {
                        TSNode nameNode = capture.node;
                        int input_count = 1;
                        std::unordered_map<std::string, std::string> localPlaceholderMap;
                        int localCounter = 1;
                        ExpressionResult res = FormalizeExpression(capture.node, code, localPlaceholderMap, -1, localCounter);
                        exp = res.template_text;
                        in.insert(in.end(), res.extracted_ports.begin(), res.extracted_ports.end());
                    }
                }
                ContinuousAssignNode* an = arena.make<ContinuousAssignNode>(id, exp);
                an->in_ports = in;
                an->out_ports.push_back(out);
                an = static_cast<ContinuousAssignNode*>(this->AddChild(newParent, an));
                outMap[an] = std::make_tuple(ts_node_start_point(currentNode).row, ts_node_end_point(currentNode).row);
                newParent = an;
            }
        }
    }

    if (ts_tree_cursor_goto_first_child(cursor)) {
        // 递归子节点
        do {
            UpdateTreeFromTS(cursor, newParent, filePath, code, outMap);
        } while (ts_tree_cursor_goto_next_sibling(cursor));

        // 处理完所有子节点后，务必跳回父节点
        ts_tree_cursor_goto_parent(cursor);
    }
}

TopNodeType isTSNodeTop(std::string node_type) {
    if (node_type == "module_declaration") return TopNodeType::Module;
    else if (node_type == "udp_declaration") return TopNodeType::UDP;
    else return TopNodeType::Null;
}

SecondNodeType isTSNodeSecond(std::string node_type) {
    if (node_type == "module_instantiation") return SecondNodeType::ModuleInstance;
    else if (node_type == "gate_instantiation") return SecondNodeType::GateInstance;
    else if (node_type == "always_construct") return SecondNodeType::Always;
    else if (node_type == "continuous_assign") return SecondNodeType::ContinuousAssign;
    else return SecondNodeType::Null;
}

bool isTSNodeNet(std::string node_type) {
    if (node_type == "net_declaration") return true;
    else return false;
}

void SigFlowTree::ConstructDefinitionTable() {
    for (SigTreeNode* sig : root->GetChildren()) {
        for (SigTreeNode* s : sig->GetChildren()) {
            TopNode* def = static_cast<TopNode*>(s);
            DefinitionTable.emplace(def->identifier, def);
        }
    }
}

void SigFlowTree::ConstructInstanceTable() {
    for (SigTreeNode* sig : root->GetChildren()) {
        for (SigTreeNode* si : sig->GetChildren()) {
            for (SigTreeNode* s : si->GetChildren()) {
                SecondNode* inst = static_cast<SecondNode*>(s);
                if (inst->secondType == SecondNodeType::ModuleInstance) {
                    InstanceTable.emplace(inst->identifier, static_cast<ModuleInstNode*>(inst));
                }
            }
        }
    }
}

std::vector<std::string> SigFlowTree::GetDefinitions() {
    std::vector<std::string> defs;
    defs.reserve(DefinitionTable.size());
    for (auto const& [name, node] : DefinitionTable) {
        defs.push_back(name);
    }
    return defs;
}

void SigFlowTree::LinkInstsWithDefs() {
    for (auto& pair : InstanceTable) {
        LinkSingleInstWithDef(pair.second);
    }
}

void SigFlowTree::LinkSingleInstWithDef(ModuleInstNode* inst) {
    HangInst(inst);
    if (!inst) return;
    inst->in_ports.clear();
    inst->out_ports.clear();
    auto it = DefinitionTable.find(inst->defIdentifier);

    if (it != DefinitionTable.end()) {
        TopNode* def = it->second;
        inst->Definition = def;

        if (inst->inout_ports.size() > 0) {
            for (auto& instPort : inst->inout_ports) {
                auto defInPortIt = std::find_if(def->GetInPorts().begin(), def->GetInPorts().end(),
                    [&instPort](const Port& p) {
                        return p.identifier == instPort.identifier;
                    });
                auto defOutPortIt = std::find_if(def->GetOutPorts().begin(), def->GetOutPorts().end(),
                    [&instPort](const Port& p) {
                        return p.identifier == instPort.identifier;
                    });

                if (defInPortIt != def->GetInPorts().end()) {
                    instPort.direction = defInPortIt->direction;
                    inst->in_ports.push_back(instPort);
                }
                else if (defOutPortIt != def->GetOutPorts().end()) {
                    instPort.direction = defOutPortIt->direction;
                    inst->out_ports.push_back(instPort);
                }
            }
            inst->inout_ports.clear();
        }
        else {
            for (auto p : def->GetInPorts()) {
                inst->in_ports.push_back(p);
            }
            for (auto p : def->GetOutPorts()) {
                inst->out_ports.push_back(p);
            }
        }
    }
}

void SigFlowTree::HangInst(ModuleInstNode* inst) {
    inst->Definition = nullptr;
    for (auto& port : inst->out_ports) {
        port.direction = PortDirection::InOut;
        inst->inout_ports.push_back(port);
    }
    inst->out_ports.clear();
    for (auto& port : inst->in_ports) {
        port.direction = PortDirection::InOut;
        inst->inout_ports.push_back(port);
    }
    inst->in_ports.clear();
}

void SigFlowTree::UpdateTreeFromSlang(slang::ast::Compilation* compilation) {
    // Not implemented
}

void SigFlowTree::PrintTree() {
    OutputDebugStringA("######################## SigTree ########################\n");

    root->Print();
    OutputDebugStringA("\n");

    for (SigTreeNode* node : root->GetChildren()) {
        FileNode* fn = static_cast<FileNode*>(node);
        fn->Print();
        OutputDebugStringA("\n");
        for (SigTreeNode* child : fn->GetChildren()) {
            TopNode* tn = static_cast<TopNode*>(child);
            tn->Print();
            OutputDebugStringA("\n");
            for (SigTreeNode* child2 : tn->GetChildren()) {
                if (child2->type == SigTreeNodeType::Second) {
                    SecondNode* sn = static_cast<SecondNode*>(child2);
                    sn->Print();
                    OutputDebugStringA("\n");
                }
                else if (child2->type == SigTreeNodeType::Signal) {
                    SignalNode* nn = static_cast<SignalNode*>(child2);
                    nn->Print();
                    OutputDebugStringA("\n");
                }
            }
        }
    };
}

void SigTreeNode::Print() {
    std::string info;

    info += "Type: ";
    switch (type) {
    case SigTreeNodeType::Project:
        info += "Project";
        break;
    case SigTreeNodeType::File:
        info += "File";
        break;
    case SigTreeNodeType::Top:
        info += "Top";
        break;
    case SigTreeNodeType::Second:
        info += "Second";
        break;
    case SigTreeNodeType::Signal:
        info += "Signal";
        break;
    default:
        info += "Unknown";
    }
    info += "\n";

    OutputDebugStringA(info.c_str());
}

void SigTreeNode::ClearNode() {
    for (auto child : children) {
        if (child) {
            child->ClearNode();
            delete child;
        }
    }
    children.clear();
}

std::string SigTreeNode::ToVerilog() {
    return std::format("");
}

std::string ProjectNode::ToVerilog() {
    return std::format("");
}

std::string FileNode::ToVerilog() {
    std::string v;
    for (auto* child : GetChildren()) {
        v += child->ToVerilog();
    }
    return v;
}

std::string TopNode::ToVerilog() {
    std::string v = std::format("module {}", identifier);
    v += "(";
    for (auto port : in_ports) {
        v += std::format("{} {},\n", port.portDirectionToStr(port.direction), port.identifier);
    }
    for (auto port : out_ports) {
        v += std::format("{} {},\n", port.portDirectionToStr(port.direction), port.identifier);
    }
    v += ")\n";

    for (auto* child : GetChildren()) {
        v += child->ToVerilog();
    }

    v += "endmodule\n";
    return v;
}

std::string SignalNode::ToVerilog() {
    std::string v;
    switch (signalType) {
    case SignalType::Wire:
        v += std::format("wire {};\n", identifier);
        break;
    case SignalType::Reg:
        v += std::format("reg {};\n", identifier);
        break;
    case SignalType::Logic:
        v += std::format("logic {};\n", identifier);
    }
    return v;
}

// 删除所有与 nb_or_b_expressions 相关的旧方法
// DelExpressionsPort, AddExpressionsPort, AddEmptyExpression, DelLastExpression 已移除

std::string AlwaysNode::GetName() {
    return SigFlowTree::ToString(secondType) + " " + identifier;
}

void AlwaysNode::Print() {
    std::string info;
    info += "SecondType: Always\n";
    info += "Edge: " + std::string(edgeType == EdgeType::Posedge ? "posedge" : "negedge") + "\n";
    info += "Statement count: " + std::to_string(getStatementCount()) + "\n";
    OutputDebugStringA(info.c_str());
    SecondNode::Print();
}

//=============================================================================
// AlwaysNode 新增接口实现（StatementSequence）
//=============================================================================

void AlwaysNode::addStatement(std::unique_ptr<Statement> stmt) {
    statements_.push_back(std::move(stmt));
}

void AlwaysNode::insertStatement(size_t index, std::unique_ptr<Statement> stmt) {
    if (index <= statements_.size()) {
        statements_.insert(statements_.begin() + index, std::move(stmt));
    }
}

void AlwaysNode::removeStatement(size_t index) {
    if (index < statements_.size()) {
        statements_.erase(statements_.begin() + index);
    }
}

const Statement* AlwaysNode::getStatement(size_t index) const {
    if (index < statements_.size()) {
        return statements_[index].get();
    }
    return nullptr;
}

size_t AlwaysNode::getStatementCount() const {
    return statements_.size();
}

void AlwaysNode::updateSignalName(const std::string& oldName, const std::string& newName) {
    StatementSequence::updateSignalName(oldName, newName);
}

SigTreeNode* AlwaysNode::Clone(Arena& arena) const {
    auto* copy = arena.make<AlwaysNode>(identifier, edgeType);
    copy->in_ports = this->in_ports;
    copy->out_ports = this->out_ports;
    for (const auto& stmt : statements_) {
        copy->statements_.push_back(stmt->clone());
    }
    return copy;
}

std::string ProjectNode::GetName() { return "Project " + std::filesystem::path(projectPath).filename().string(); }
std::string FileNode::GetName() { return "File " + std::filesystem::path(filePath).filename().string(); }
std::string TopNode::GetName() { return SigFlowTree::ToString(topType) + " " + identifier; }
std::string SecondNode::GetName() { return SigFlowTree::ToString(secondType) + " " + identifier; };
std::string SignalNode::GetName() { return SigFlowTree::ToString(signalType) + " " + identifier; }

void ProjectNode::Print() {
    SigTreeNode::Print();
    std::string info;
    info = "ProjectPath: " + projectPath + "\n";
    OutputDebugStringA(info.c_str());
}

void FileNode::Print() {
    SigTreeNode::Print();
    std::string info;
    info = "FilePath: " + filePath + "\n";
    OutputDebugStringA(info.c_str());
}

void TopNode::Print() {
    SigTreeNode::Print();
    std::string info;
    info = "Identifier: " + identifier + "\n";

    switch (topType) {
    case TopNodeType::Module:
        info += "TopType: Module\n";
        break;
    case TopNodeType::UDP:
        info += "TopType: UDP\n";
        break;
    default:
        info += "TopType: Unknown\n";
    }

    for (Port& p : in_ports) {
        info += "port: " + p.identifier + " " + "In" + "\n";
    }
    for (Port& p : out_ports) {
        info += "port: " + p.identifier + " " + "Out" + "\n";
    }

    OutputDebugStringA(info.c_str());
}

void SecondNode::Print() {
    SigTreeNode::Print();
    std::string info;
    info = "Identifier: " + identifier + "\n";

    for (Port& p : in_ports) {
        info += "port: " + p.identifier + " " + "In" + " to: " + p.conn + "\n";
    }

    for (Port& p : out_ports) {
        info += "port: " + p.identifier + " " + "Out" + " to: " + p.conn + "\n";
    }

    OutputDebugStringA(info.c_str());
}

void SignalNode::Print() {
    SigTreeNode::Print();
    std::string info;
    info += "Signal Type: " + SigFlowTree::ToString(signalType) + "\n";
    info += "Identifier: " + identifier + "\n";

    OutputDebugStringA(info.c_str());
}

void SigTreeNode::RemoveChildren() {
    for (auto* cld : children) {
        RemoveChild(cld);
    }
}

void SigTreeNode::RemoveChild(SigTreeNode* child) {
    auto it = std::find(children.begin(), children.end(), child);
    if (it == children.end())
        return;
    children.erase(it);
}

bool SigTreeNode::AddChild(SigTreeNode* child) {
    if (!child) return false;

    if (this->CanBeChild(child->type) && child->CanBeParent(this->type)) {
        if (child->parent == this) return false;
        if (child->parent) {
            child->parent->RemoveChild(child);
        }
        child->parent = this;
        children.push_back(child);
        return true;
    }
    else return false;
}

void SigFlowTree::ClearTree() {
    DefinitionTable.clear();
    InstanceTable.clear();
    SignalTable.clear();
    root = nullptr;
    arena.reset();
}

void SigFlowTree::RemoveChild(SigTreeNode* parent, SigTreeNode* child) {
    if (!parent || !child) return;

    UnregisterNodeRecursive(child);
    parent->RemoveChild(child);
    wxCommandEvent evt(EVT_SIGFLOWNODE_DEL);
    evt.SetClientData(child);
    wxPostEvent(m_parent->GetEventHandler(), evt);
}

SigTreeNode* SigFlowTree::AddChild(SigTreeNode* parent, SigTreeNode* externalNode) {
    if (!externalNode) return nullptr;

    if (parent) {
        if (!parent->CanBeChild(externalNode->type) ||
            !externalNode->CanBeParent(parent->type)) {
            return nullptr;
        }
    }
    else {
        if (externalNode->type != SigTreeNodeType::Project) {
            return nullptr;
        }
    }

    SigTreeNode* safeNode = CloneSubtreeToArena(externalNode);

    if (parent) {
        parent->AddChild(safeNode);
        wxCommandEvent event(EVT_SIGFLOWNODE_ADD);
        event.SetClientData(safeNode);
        m_parent->GetEventHandler()->ProcessEvent(event);
    }
    else {
        this->root = static_cast<ProjectNode*>(safeNode);
    }

    RegisterNodeRecursive(safeNode);
    return safeNode;
}

SigTreeNode* SigFlowTree::CloneSubtreeToArena(SigTreeNode* node) {
    if (!node) return nullptr;

    SigTreeNode* newNode = node->Clone(this->arena);

    for (auto child : node->GetChildren()) {
        SigTreeNode* newChild = CloneSubtreeToArena(child);
        newNode->AddChild(newChild);
    }
    return newNode;
}

void SigFlowTree::AddInPort(SecondNode* sn) {
    std::string name = "in" + std::to_string(sn->in_ports.size() + 1);
    Port p(name, PortDirection::In);
    sn->in_ports.push_back(p);
    wxCommandEvent evt(EVT_SIGFLOWNODE_CHANGED);
    evt.SetClientData(sn);
    wxPostEvent(m_parent->GetEventHandler(), evt);
}

void SigFlowTree::AddOutPort(SecondNode* sn) {
    std::string name = "out" + std::to_string(sn->out_ports.size() + 1);
    Port p(name, PortDirection::Out);
    wxCommandEvent evt(EVT_SIGFLOWNODE_CHANGED);
    sn->out_ports.push_back(p);
    evt.SetClientData(sn);
    wxPostEvent(m_parent->GetEventHandler(), evt);
}

void SigFlowTree::AddInPort(TopNode* tn) {
    std::string name = "in" + std::to_string(tn->in_ports.size() + 1);
    Port p(name, PortDirection::In);
    tn->in_ports.push_back(p);
    wxCommandEvent evt(EVT_SIGFLOWNODE_CHANGED);
    evt.SetClientData(tn);
    wxPostEvent(m_parent->GetEventHandler(), evt);
}

void SigFlowTree::AddOutPort(TopNode* tn) {
    std::string name = "out" + std::to_string(tn->out_ports.size() + 1);
    Port p(name, PortDirection::Out);
    wxCommandEvent evt(EVT_SIGFLOWNODE_CHANGED);
    tn->out_ports.push_back(p);
    evt.SetClientData(tn);
    wxPostEvent(m_parent->GetEventHandler(), evt);
}

void SigFlowTree::AddInOutPort(SecondNode* sn) {
    // Not implemented
}

void SigFlowTree::SecondDelLastInPort(SecondNode* sn) {
    if (!sn->in_ports.empty())
        sn->in_ports.pop_back();
    wxCommandEvent evt(EVT_SIGFLOWNODE_CHANGED);
    evt.SetClientData(sn);
    wxPostEvent(m_parent->GetEventHandler(), evt);
}

void SigFlowTree::TopDelPort(TopNode* sn, Port p) {
    if (p.direction == PortDirection::In) {
        auto it = std::find_if(sn->GetInPorts().begin(), sn->GetInPorts().end(),
            [&](const Port& item) {
                return item.identifier == p.identifier;
            });
        if (it != sn->GetInPorts().end()) {
            sn->GetInPorts().erase(it);
            wxCommandEvent evt(EVT_SIGFLOWNODE_CHANGED);
            evt.SetClientData(sn);
            m_parent->GetEventHandler()->ProcessEvent(evt);
        }
    }
    else if (p.direction == PortDirection::Out) {
        auto it = std::find_if(sn->GetOutPorts().begin(), sn->GetOutPorts().end(),
            [&](const Port& item) {
                return item.identifier == p.identifier;
            });
        if (it != sn->GetOutPorts().end()) {
            sn->GetOutPorts().erase(it);
            wxCommandEvent evt(EVT_SIGFLOWNODE_CHANGED);
            evt.SetClientData(sn);
            m_parent->GetEventHandler()->ProcessEvent(evt);
        }
    }
}

void SigFlowTree::TopDelPort(TopNode* sn, wxString port_id) {
    auto it = std::find_if(sn->GetInPorts().begin(), sn->GetInPorts().end(),
        [&](const Port& item) {
            return item.identifier == port_id;
        });
    if (it != sn->GetInPorts().end()) {
        sn->GetInPorts().erase(it);
        wxCommandEvent evt(EVT_SIGFLOWNODE_CHANGED);
        evt.SetClientData(sn);
        m_parent->GetEventHandler()->ProcessEvent(evt);
    }

    auto it2 = std::find_if(sn->GetOutPorts().begin(), sn->GetOutPorts().end(),
        [&](const Port& item) {
            return item.identifier == port_id;
        });
    if (it2 != sn->GetOutPorts().end()) {
        sn->GetOutPorts().erase(it2);
        wxCommandEvent evt(EVT_SIGFLOWNODE_CHANGED);
        evt.SetClientData(sn);
        m_parent->GetEventHandler()->ProcessEvent(evt);
    }
}

void SigFlowTree::PortReName(TopNode* tn, wxString old_id, wxString new_id) {
    auto findAndRename = [&](std::vector<Port>& ports) -> bool {
        auto it = std::find_if(ports.begin(), ports.end(),
            [&](const Port& item) { return item.identifier == old_id; });
        if (it != ports.end()) {
            it->identifier = new_id;
            return true;
        }
        return false;
        };

    bool changed = findAndRename(tn->GetInPorts());
    if (!changed) {
        changed = findAndRename(tn->GetOutPorts());
    }

    if (changed) {
        wxCommandEvent* evt = new wxCommandEvent(EVT_SIGFLOWNODE_CHANGED);
        evt->SetClientData(tn);
        m_parent->GetEventHandler()->QueueEvent(evt);
    }
}

void SigFlowTree::PortReName(SecondNode* sn, wxString old_id, wxString new_id) {
    std::string oldStr = old_id.ToStdString();
    std::string newStr = new_id.ToStdString();

    // 先尝试在 in_ports 和 out_ports 中查找并重命名
    bool changed = false;
    auto findAndRename = [&](std::vector<Port>& ports) -> bool {
        auto it = std::find_if(ports.begin(), ports.end(),
            [&](const Port& item) { return item.identifier == oldStr; });
        if (it != ports.end()) {
            it->identifier = newStr;
            return true;
        }
        return false;
        };

    changed = findAndRename(sn->in_ports);
    if (!changed) {
        changed = findAndRename(sn->out_ports);
    }

    // 如果是 AlwaysNode，还需要更新语句中的引用
    if (sn->secondType == SecondNodeType::Always) {
        AlwaysNode* an = static_cast<AlwaysNode*>(sn);
        for (size_t i = 0; i < an->getStatementCount(); ++i) {
            const Statement* stmtBase = an->getStatement(i);
            auto* stmt = const_cast<AlwaysStatement*>(dynamic_cast<const AlwaysStatement*>(stmtBase));
            if (!stmt) continue;

            // 更新输出端口名
            if (stmt->out_port_name == oldStr) {
                stmt->out_port_name = newStr;
                changed = true;
            }

            // 更新输入端口名列表
            for (auto& name : stmt->in_port_names) {
                if (name == oldStr) {
                    name = newStr;
                    changed = true;
                }
            }
        }
    }

    if (changed) {
        wxCommandEvent* evt = new wxCommandEvent(EVT_SIGFLOWNODE_CHANGED);
        evt->SetClientData(sn);
        m_parent->GetEventHandler()->QueueEvent(evt);
    }
}

void SigFlowTree::PortConn(SecondNode* sn, wxString id, wxString conn) {
    if (!sn) return;

    bool found = false;

    for (auto& p : sn->in_ports) {
        if (p.identifier == id) {
            p.conn = conn;
            found = true;
            break;
        }
    }

    if (!found) {
        for (auto& p : sn->out_ports) {
            if (p.identifier == id) {
                p.conn = conn;
                found = true;
                break;
            }
        }
    }

    if (found) {
        wxCommandEvent* evt = new wxCommandEvent(EVT_SIGFLOWNODE_CHANGED);
        evt->SetClientData(sn);
        m_parent->GetEventHandler()->QueueEvent(evt);
    }
}

void SigFlowTree::ReIdentifier(TopNode* tn, wxString id) {
    tn->identifier = id;
    wxCommandEvent* evt = new wxCommandEvent(EVT_SIGFLOWNODE_CHANGED);
    evt->SetClientData(tn);
    m_parent->GetEventHandler()->QueueEvent(evt);
}

void SigFlowTree::ReIdentifier(SecondNode* sn, wxString id) {
    sn->identifier = id;
    wxCommandEvent* evt = new wxCommandEvent(EVT_SIGFLOWNODE_CHANGED);
    evt->SetClientData(sn);
    m_parent->GetEventHandler()->QueueEvent(evt);
}

void SigFlowTree::ReIdentifier(SignalNode* sn, wxString id) {
    sn->identifier = id;
    wxCommandEvent* evt = new wxCommandEvent(EVT_SIGFLOWNODE_CHANGED);
    evt->SetClientData(sn);
    m_parent->GetEventHandler()->QueueEvent(evt);
}

void SigFlowTree::RegisterNodeRecursive(SigTreeNode* node) {
    if (!node) return;

    if (node->type == SigTreeNodeType::Top) {
        TopNode* tn = static_cast<TopNode*>(node);
        DefinitionTable[tn->identifier] = tn;

        for (auto& pair : InstanceTable) {
            if (pair.second->defIdentifier == tn->identifier) {
                LinkSingleInstWithDef(pair.second);
            }
        }
    }
    else if (node->type == SigTreeNodeType::Second) {
        SecondNode* sn = static_cast<SecondNode*>(node);
        if (sn->secondType == SecondNodeType::ModuleInstance) {
            ModuleInstNode* mn = static_cast<ModuleInstNode*>(sn);
            InstanceTable[mn->identifier] = mn;
            LinkSingleInstWithDef(mn);
        }
    }
    else if (node->type == SigTreeNodeType::Signal) {
        SignalNode* sn = static_cast<SignalNode*>(node);
        SignalTable.emplace(sn->identifier, sn);
    }

    for (auto child : node->GetChildren()) {
        RegisterNodeRecursive(child);
    }
}

void SigFlowTree::UnregisterNodeRecursive(SigTreeNode* node) {
    if (!node) return;

    switch (node->type) {
    case SigTreeNodeType::Top: {
        TopNode* top = static_cast<TopNode*>(node);
        DefinitionTable.erase(top->identifier);
        for (auto& pair : InstanceTable) {
            if (pair.second->Definition == top) {
                HangInst(pair.second);
            }
        }
        break;
    }
    case SigTreeNodeType::Second: {
        SecondNode* inst = static_cast<SecondNode*>(node);
        InstanceTable.erase(inst->identifier);
        break;
    }
    case SigTreeNodeType::Signal: {
        // 如果有 SignalTable，在这里 erase
        break;
    }
    default: break;
    }

    for (auto child : node->GetChildren()) {
        UnregisterNodeRecursive(child);
    }
}

FileNode* SigFlowTree::GetFileNode(std::string filePath) {
    for (SigTreeNode* cld : root->GetChildren()) {
        FileNode* fn = static_cast<FileNode*>(cld);
        if (fn->filePath == filePath) return fn;
    }
    return nullptr;
}

std::vector<int> SigFlowTree::SecondNodeTopoLevel(TopNode* tn) {
    if (!tn) return {};

    std::vector<SecondNode*> nodes;
    for (auto* child : tn->GetChildren()) {
        if (child->type == SigTreeNodeType::Second)
            nodes.push_back(static_cast<SecondNode*>(child));
    }

    int n = nodes.size();
    std::vector<int> indegree(n, 0);
    std::vector<int> level(n, 0);

    std::unordered_map<SecondNode*, int> index;
    for (int i = 0; i < n; ++i)
        index[nodes[i]] = i;

    std::vector<std::vector<int>> adj(n);

    for (int i = 0; i < n; ++i) {
        for (int j = 0; j < n; ++j) {
            if (i == j) continue;

            for (auto& p_out : nodes[i]->out_ports) {
                for (auto& p_in : nodes[j]->in_ports) {
                    if (!p_out.conn.empty() && p_out.conn == p_in.conn) {
                        adj[i].push_back(j);
                        indegree[j]++;
                    }
                }
            }
        }
    }

    std::queue<int> q;

    for (int i = 0; i < n; ++i) {
        if (indegree[i] == 0) {
            level[i] = 0;
            q.push(i);
        }
    }

    int processed = 0;

    while (!q.empty()) {
        int u = q.front();
        q.pop();
        processed++;

        for (int v : adj[u]) {
            level[v] = std::max(level[v], level[u] + 1);
            indegree[v]--;

            if (indegree[v] == 0)
                q.push(v);
        }
    }

    if (processed < n) {
        int maxLevel = 0;
        for (int i = 0; i < n; ++i)
            maxLevel = std::max(maxLevel, level[i]);

        for (int i = 0; i < n; ++i) {
            if (indegree[i] > 0) {
                level[i] = maxLevel;
            }
        }
    }

    return level;
}

GateInstNode::GateInstNode(std::string id, GateType gt) : SecondNode(id, SecondNodeType::GateInstance), gatetype(gt) {
    in_ports.push_back(Port("in1", PortDirection::In));
    in_ports.push_back(Port("in2", PortDirection::In));
    out_ports.push_back(Port("out", PortDirection::Out));
}

std::string GateInstNode::ToVerilog() {
    std::string v = "    " + SigFlowTree::ToString(gatetype) + " " + identifier + "(";

    for (auto port : out_ports) {
        v += port.conn + ", ";

    }
    for (int i = 0; i < in_ports.size(); i++) {
        auto port = in_ports[i];
        v += port.conn;
        if (i < in_ports.size()-1) v +=", ";
    }

    v += ");\n\n";
    return v;
}

std::string GateInstNode::GetName() {
    return SigFlowTree::ToString(gatetype) + " " + identifier;
}

void GateInstNode::Print() {
    OutputDebugStringA("SecondType: GateInstance\n");
    SecondNode::Print();
}

ModuleInstNode::ModuleInstNode(std::string id, TopNode* Definition) : SecondNode(id, SecondNodeType::ModuleInstance) {
    SetDefinition(Definition);
    SetDefinition(Definition->identifier);
}

void ModuleInstNode::SetDefinition(std::string defIdentifier) {
    this->defIdentifier = defIdentifier;
}

void ModuleInstNode::SetDefinition(TopNode* Definition) {
    this->Definition = Definition;
    inout_ports.clear();
    for (auto p : Definition->GetInPorts()) {
        in_ports.push_back(p);
    }
    for (auto p : Definition->GetOutPorts()) {
        out_ports.push_back(p);
    }
}

std::string ModuleInstNode::ToVerilog() {
    std::string v = std::format("    {} {}", defIdentifier, identifier);
    v += "(\n";
    for (auto port : in_ports) {
        v += std::format("        .{}({}),\n", port.identifier, port.conn);
    }
    for (auto port : out_ports) {
        v += std::format("        .{}({}),\n", port.identifier, port.conn);
    }
    v += "    );\n\n";

    return v;
}

std::string ModuleInstNode::GetName() {
    return defIdentifier + " " + identifier;
}

void ModuleInstNode::Print() {
    std::string info;
    info += "SecondType: ModuleInstance\n";
    info += "Definition: " + defIdentifier + "\n";
    for (Port& p : inout_ports) {
        info += "port: " + p.identifier + " " + "InOut" + " to: " + p.conn + "\n";
    }
    OutputDebugStringA(info.c_str());
    SecondNode::Print();
}

ContinuousAssignNode::ContinuousAssignNode(std::string id, std::string raw_assign) : SecondNode(id, SecondNodeType::ContinuousAssign) {
    // 构造函数，可根据需要解析 raw_assign
}

std::string ContinuousAssignNode::ToVerilog() {
    std::string v = std::format("assign ");
    for (auto port : out_ports) {
        v += port.conn + " = ";
    }
    for (auto port : in_ports) {
        v += port.conn + " = ";
    }
    v += ";\n";
    return v;
}

std::string ContinuousAssignNode::GetName() {
    return SigFlowTree::ToString(secondType) + " " + out_ports[0].conn;
}

void ContinuousAssignNode::Print() {
    std::string info;
    info += "SecondType: ContinuousAssign\n";
    info += "Expression: " + template_exp + "\n";
    OutputDebugStringA(info.c_str());
    SecondNode::Print();
}

//=============================================================================
// AlwaysNode 新增接口实现（基于信号名称）
//=============================================================================

void AlwaysNode::AddEmptyExpression() {
    // 生成默认输出端口名（确保唯一）
    std::string outName = "out" + std::to_string(out_ports.size() + 1);
    out_ports.push_back(Port(outName, PortDirection::Out, ""));

    // 创建默认 AlwaysStatement
    auto stmt = std::make_unique<AlwaysStatement>(
        false,          // 默认非阻塞
        "",             // 空表达式
        0.0f,           // 无延迟
        outName,        // 输出端口名
        std::vector<std::string>{} // 输入端口列表为空
    );

    addStatement(std::move(stmt));
}

void AlwaysNode::DelLastExpression() {
    if (getStatementCount() > 0) {
        size_t lastIdx = getStatementCount() - 1;
        auto* stmt = dynamic_cast<const AlwaysStatement*>(getStatement(lastIdx));
        if (stmt) {
            std::string outName = stmt->out_port_name;
            removeStatement(lastIdx);
            // 同步删除输出端口
            auto& out = out_ports;
            out.erase(std::remove_if(out.begin(), out.end(),
                [&outName](const Port& p) { return p.identifier == outName; }),
                out.end());
        }
    }
}

void AlwaysNode::AddPortToExpression(int exp_id) {
    if (exp_id < 0 || exp_id >= static_cast<int>(statements_.size())) return;

    auto* stmt = dynamic_cast<AlwaysStatement*>(statements_[exp_id].get());
    if (!stmt) return;

    AddPortToExpression(stmt);
}

void AlwaysNode::DeletePort(int portIndex) {
    if (portIndex < 0 || portIndex >= static_cast<int>(in_ports.size())) return;

    // 获取要删除的端口名
    std::string portName = in_ports[portIndex].identifier;

    // 从 in_ports 中删除该端口
    in_ports.erase(in_ports.begin() + portIndex);

    // 遍历所有语句，从它们的 in_port_names 中移除该端口名
    for (auto& stmtPtr : statements_) {
        auto* stmt = dynamic_cast<AlwaysStatement*>(stmtPtr.get());
        if (!stmt) continue;
        auto& names = stmt->in_port_names;
        names.erase(std::remove(names.begin(), names.end(), portName), names.end());
    }

    // 注意：如果某语句的输出端口名恰好等于 portName，此处不处理（输出端口仅通过表达式删除间接移除）
}

// 更新 ToVerilog 以使用 statements_（原代码已正确，无需修改）
std::string AlwaysNode::ToVerilog() {
    std::string v = "always @(";
    v += (edgeType == EdgeType::Posedge) ? "posedge " : "negedge ";

    // 从输入端口中提取时钟信号的实际连接名
    std::string clkSignal;
    for (const auto& port : in_ports) {
        if (port.identifier == "CLK") {
            clkSignal = port.conn;
            break;
        }
    }
    if (clkSignal.empty()) clkSignal = identifier; // 后备
    v += clkSignal + ") begin\n";

    for (size_t i = 0; i < statements_.size(); ++i) {
        auto* stmt = dynamic_cast<AlwaysStatement*>(statements_[i].get());
        if (!stmt) continue;

        // 替换表达式中的输入占位符
        std::string expr = stmt->nb_or_b_expression;
        for (const auto& port : in_ports) {
            size_t pos = 0;
            while ((pos = expr.find(port.identifier, pos)) != std::string::npos) {
                expr.replace(pos, port.identifier.length(), port.conn);
                pos += port.conn.length();
            }
        }

        // 查找输出端口对应的实际信号名
        std::string outSignal = stmt->out_port_name;
        for (const auto& port : out_ports) {
            if (port.identifier == stmt->out_port_name) {
                outSignal = port.conn;
                break;
            }
        }

        if (stmt->delay > 0.0f)
            v += "    #" + std::to_string(stmt->delay) + " ";
        else
            v += "    ";

        v += outSignal + " ";
        v += (stmt->is_blocking ? "= " : "<= ");
        v += expr + ";\n";
    }
    v += "end\n";
    return v;
}
void AlwaysNode::CleanUnusedInPorts() {
    std::unordered_set<std::string> usedInPorts;
    for (size_t i = 0; i < getStatementCount(); ++i) {
        const Statement* stmtBase = getStatement(i);
        auto* stmt = dynamic_cast<const AlwaysStatement*>(stmtBase);
        if (!stmt) continue;
        usedInPorts.insert(stmt->in_port_names.begin(), stmt->in_port_names.end());
    }

    in_ports.erase(std::remove_if(in_ports.begin(), in_ports.end(),
        [&](const Port& p) {
            // 保留时钟端口 "CLK"（即使未使用）
            return p.identifier != "CLK" && usedInPorts.find(p.identifier) == usedInPorts.end();
        }), in_ports.end());
}

void AlwaysNode::RemoveExpression(size_t index) {
    if (index >= statements_.size()) return;

    // 获取要删除的语句，并转成 AlwaysStatement 以便访问输出端口名
    auto* stmt = dynamic_cast<AlwaysStatement*>(statements_[index].get());
    if (!stmt) return;

    std::string outName = stmt->out_port_name;

    // 删除语句
    removeStatement(index);

    // 从输出端口中移除对应的端口（假设输出端口名唯一）
    auto& outs = out_ports;
    outs.erase(std::remove_if(outs.begin(), outs.end(),
        [&outName](const Port& p) { return p.identifier == outName; }),
        outs.end());

    // 清理无用的输入端口
    CleanUnusedInPorts();
}

void AlwaysNode::AddPortToExpression(AlwaysStatement* stmt) {
    if (!stmt) return;

    // 生成唯一输入端口名
    std::string inName = "in" + std::to_string(in_ports.size() + 1);
    in_ports.push_back(Port(inName, PortDirection::In, ""));

    // 将端口名加入语句的输入列表
    stmt->in_port_names.push_back(inName);
}

void AlwaysNode::DeletePort(const std::string& portName) {
    for (size_t i = 0; i < in_ports.size(); ++i) {
        if (in_ports[i].identifier == portName) {
            DeletePort(static_cast<int>(i));
            return;
        }
    }
}
