#include "SigTree.h"

#include <json/json.h>
#include <iostream>
#include <fstream>
#include <filesystem>
#include <memory>
#include <windows.h>
#include <regex>
#include <set>
#include <queue>

extern "C" TSLanguage* tree_sitter_verilog();

namespace fs = std::filesystem;

TopNodeType isTSNodeTop(std::string node_type);
SecondNodeType isTSNodeSecond(std::string node_type);
bool isTSNodeNet(std::string node_type);
void CollectSigTreeNodeInfoTS(SigTreeNode* node, TSNode& TSnode, std::string filepath, std::string code);

SigFlowTree::SigFlowTree() {
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

ExpressionResult FormalizeExpression(TSNode node, const std::string& code, int &in_counter, int exp_id) {
    ExpressionResult result;
    const char* type = ts_node_type(node);

    // 1. 如果是变量标识符：将其替换为占位符，不进行去重
    if (strcmp(type, "simple_identifier") == 0) {
        std::string original_conn = code.substr(ts_node_start_byte(node),
            ts_node_end_byte(node) - ts_node_start_byte(node));

        std::string placeholder;

        // 生成唯一的占位符名，例如 In1, In2...
        if (exp_id != -1) {
            placeholder = "In" + std::to_string(exp_id) + "_" + std::to_string(in_counter++);
        }
        else {
            placeholder = "In" + std::to_string(in_counter++);
        }
        

        result.template_text = placeholder;

        // 创建对应的 Port 对象
        Port p;
        p.identifier = placeholder;
        p.conn = original_conn;
        p.direction = PortDirection::In;
        result.extracted_ports.push_back(p);

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
        ExpressionResult child_res = FormalizeExpression(child, code, in_counter, exp_id);

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

void SigFlowTree::UpdateTreeFromTS(TSTreeCursor* cursor, SigTreeNode* SigRoot,std::string& filePath, std::string& code) {
    TSNode currentNode = ts_tree_cursor_current_node(cursor);
    SigTreeNode* newParent = SigRoot;
    std::string node_type = ts_node_type(currentNode);

    if (ts_node_is_extra(currentNode)) {
        if (ts_tree_cursor_goto_first_child(cursor)) {
            // 递归子节点
            do {
                UpdateTreeFromTS(cursor, newParent, filePath, code);
            } while (ts_tree_cursor_goto_next_sibling(cursor));

            // 处理完所有子节点后，务必跳回父节点
            ts_tree_cursor_goto_parent(cursor);
        }
        return;
    }

    // 首先判断parent类型
    if (newParent->type == SigTreeNodeType::Project) {
        // 找File
        std::string node_type = ts_node_type(currentNode);
        if (node_type == "source_file") {
            FileNode* fn = arena.make<FileNode>(filePath);
            fn->parent = newParent;
            CollectSigTreeNodeInfoTS(fn, currentNode, filePath, code);
            newParent->children.push_back(fn);
            newParent = fn;
        }
    }
    else if (newParent->type == SigTreeNodeType::File) {
        // 找Top
        TopNodeType top_type = isTSNodeTop(node_type);
        if (top_type != TopNodeType::Null) {
            TopNode* tn = arena.make<TopNode>();
            tn->topType = top_type;


            // 找Port和ID
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
                        tn->identifier = code.substr(ts_node_start_byte(nameNode),
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
                        tn->ports.push_back(p);
                    }
                }
            }

            ts_query_cursor_delete(cursor);


            tn->parent = newParent;
            CollectSigTreeNodeInfoTS(tn, currentNode, filePath, code);

            DefinitionTable.emplace(tn->identifier, tn);
            newParent->children.push_back(tn);
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
            while (ts_query_cursor_next_match(cursor, &match)) {
                SignalNode* nn = arena.make<SignalNode>();
                for (uint16_t i = 0; i < match.capture_count; i++) {
                    TSQueryCapture capture = match.captures[i];

                    // 3. 识别零件的标签名（通过 ID 换取字符串）
                    uint32_t name_len;
                    const char* tag_name = ts_query_capture_name_for_id(net, capture.index, &name_len);

                    // 4. 根据标签名分发逻辑
                    if (strcmp(tag_name, "net.name") == 0) {
                        TSNode nameNode = capture.node;
                        nn->identifier = code.substr(ts_node_start_byte(nameNode),
                            ts_node_end_byte(nameNode) - ts_node_start_byte(nameNode));
                        CollectSigTreeNodeInfoTS(nn, currentNode, filePath, code);
                        nn->signalType = SignalType::Wire;
                        nn->parent = newParent;
                        newParent->children.push_back(nn);
                    }

                }
                SignalTable.emplace(nn->identifier, nn);
            }
            
        }
        else if (node_type == "data_declaration") {
            TSQueryCursor* cursor = ts_query_cursor_new();
            ts_query_cursor_exec(cursor, net, currentNode);

            TSQueryMatch match;
            while (ts_query_cursor_next_match(cursor, &match)) {
                SignalNode* nn = arena.make<SignalNode>();
                for (uint16_t i = 0; i < match.capture_count; i++) {
                    TSQueryCapture capture = match.captures[i];

                    // 3. 识别零件的标签名（通过 ID 换取字符串）
                    uint32_t name_len;
                    const char* tag_name = ts_query_capture_name_for_id(net, capture.index, &name_len);

                    // 4. 根据标签名分发逻辑
                    if (strcmp(tag_name, "net.name") == 0) {
                        TSNode nameNode = capture.node;
                        nn->identifier = code.substr(ts_node_start_byte(nameNode),
                            ts_node_end_byte(nameNode) - ts_node_start_byte(nameNode));
                        CollectSigTreeNodeInfoTS(nn, currentNode, filePath, code);
                        nn->signalType = SignalType::Reg;
                        nn->parent = newParent;
                        newParent->children.push_back(nn);
                    }

                }
                SignalTable.emplace(nn->identifier, nn);
            }
        }
        // 找Second
        else if (second_type != SecondNodeType::Null) {
            SecondNode* sn = arena.make<SecondNode>();
            sn->secondType = second_type;

            if (second_type == SecondNodeType::ModuleInstance) {
                TSQueryCursor* cursor = ts_query_cursor_new();
                ts_query_cursor_exec(cursor, second, currentNode);

                TSQueryMatch match;
                while (ts_query_cursor_next_match(cursor, &match)) {
                    Port p;
                    p.direction = PortDirection::InOut;

                    for (uint16_t i = 0; i < match.capture_count; i++) {
                        TSQueryCapture capture = match.captures[i];

                        // 3. 识别零件的标签名（通过 ID 换取字符串）
                        uint32_t name_len;
                        const char* tag_name = ts_query_capture_name_for_id(second, capture.index, &name_len);

                        // 4. 根据标签名分发逻辑
                        if (strcmp(tag_name, "modinst.modname") == 0) {
                            TSNode nameNode = capture.node;
                            sn->defIdentifier = code.substr(ts_node_start_byte(nameNode),
                                ts_node_end_byte(nameNode) - ts_node_start_byte(nameNode));
                        }
                        else if (strcmp(tag_name, "modinst.instname") == 0) {
                            TSNode nameNode = capture.node;
                            sn->identifier = code.substr(ts_node_start_byte(nameNode),
                                ts_node_end_byte(nameNode) - ts_node_start_byte(nameNode));
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
                            sn->ports.push_back(p);
                        }
                    }
                }
                InstanceTable.emplace(sn->identifier, sn);
            }
            else if (second_type == SecondNodeType::GateInstance) {
                TSQueryCursor* cursor = ts_query_cursor_new();
                ts_query_cursor_exec(cursor, second, currentNode);

                TSQueryMatch match;
                uint32_t capture_index;

                uint32_t last_node_id = 0;
                const char* last_tag = "";

                int gate_input_count = 1;
                int gate_output_count = 1;

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
                        sn->gatetype = text;
                    }
                    else if (strcmp(tag_name, "gate.name") == 0) {
                        sn->identifier = text;
                    }
                    else if (strcmp(tag_name, "gate.output") == 0) {
                        Port p;
                        p.identifier = std::format("out{}", gate_output_count++); // 门原语通常只有一个输出
                        p.direction = PortDirection::Out;
                        p.conn = text;
                        sn->ports.push_back(p);
                    }
                    else if (strcmp(tag_name, "gate.input") == 0) {
                        Port p;
                        p.identifier = std::format("in{}", gate_input_count++);
                        p.direction = PortDirection::In;
                        p.conn = text;
                        sn->ports.push_back(p);
                    }
                } 
            }
            else if (second_type == SecondNodeType::Always) {
                TSQueryCursor* cursor = ts_query_cursor_new();
                ts_query_cursor_exec(cursor, second, currentNode);

                TSQueryMatch match;
                uint32_t capture_index;

                uint32_t last_node_id = 0;
                const char* last_tag = "";

                int exp_input_count = 1;
                int onput_count = 1;
                NB_OR_B_Expression exp;
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

                    if (strcmp(tag_name, "clk.edge") == 0) {
                        TSNode nameNode = capture.node;
                        std::string edgeType = code.substr(ts_node_start_byte(nameNode),
                            ts_node_end_byte(nameNode) - ts_node_start_byte(nameNode));
                        if (edgeType == "posedge") {
                            sn->edgeType = EdgeType::Posedge;
                        }
                        else if (edgeType == "negedge") {
                            sn->edgeType = EdgeType::Negedge;
                        }

                    }
                    else if (strcmp(tag_name, "clk.name") == 0) {
                        TSNode nameNode = capture.node;
                        Port p;
                        // identifier 存槽位名: In1, In2...
                        p.identifier = "CLK";

                        // direction 设为输入
                        p.direction = PortDirection::In;

                        // conn 存实际连接的信号名
                        p.conn = code.substr(ts_node_start_byte(nameNode),
                            ts_node_end_byte(nameNode) - ts_node_start_byte(nameNode));

                        sn->ports.push_back(p);

                        sn->identifier = "@" + p.conn;

                    }
                    else if (strcmp(tag_name, "assign.lhs") == 0) {
                        TSNode nameNode = capture.node;
                        Port p;
                        // identifier 存槽位名: In1, In2...
                        p.identifier = std::format("Out{}", onput_count++);

                        // direction 设为输入
                        p.direction = PortDirection::Out;

                        // conn 存实际连接的信号名
                        p.conn = code.substr(ts_node_start_byte(nameNode),
                            ts_node_end_byte(nameNode) - ts_node_start_byte(nameNode));

                        sn->ports.push_back(p);
                        exp.out_port_id = sn->ports.size() - 1;


                        TSNode father = ts_node_parent(nameNode);
                        std::string f_type = ts_node_type(father);
                        if (f_type  == "nonblocking_assignment") {
                            exp.is_blocking = false;
                        }
                        else if(f_type == "blocking_assignment"){
                            exp.is_blocking = true;
                        }
                        

                    }
                    else if (strcmp(tag_name, "assign.delay") == 0) {
                        TSNode nameNode = capture.node;
                        std::string delay = code.substr(ts_node_start_byte(nameNode),
                            ts_node_end_byte(nameNode) - ts_node_start_byte(nameNode));
                        size_t firstDigit = delay.find_first_of("0123456789.");

                        if (firstDigit != std::string::npos) {
                            try {
                                exp.delay = std::stof(delay.substr(firstDigit));
                            }
                            catch (...) {
                                exp.delay = 0.0f;
                            }
                        }
                        else {
                            exp.delay = 0.0f; // 没找到数字
                        }

                    }
                    else if (strcmp(tag_name, "assign.rhs") == 0) {
                        TSNode nameNode = capture.node;
                        // 2. 提取所有右侧变量
                        std::set<std::string> rhsSignals;
                        int input_count = 1;
                        ExpressionResult res = FormalizeExpression(capture.node, code, input_count, exp_input_count);
                        exp.nb_or_b_expression = res.template_text;
                        int startIndex = (int)sn->ports.size();
                        sn->ports.insert(sn->ports.end(), res.extracted_ports.begin(), res.extracted_ports.end());
                        int endIndex = (int)sn->ports.size() ;
                        exp.in_port_ids.clear();
                        for (int i = startIndex; i < endIndex; ++i) {
                            exp.in_port_ids.push_back(i);
                        }
                        sn->nb_or_b_expressions.push_back(exp);
                        exp_input_count++;

                    }
                }

            }
            else if (second_type == SecondNodeType::ContinuousAssign) {
                TSQueryCursor* cursor = ts_query_cursor_new();
                ts_query_cursor_exec(cursor, second, currentNode);

                TSQueryMatch match;
                uint32_t capture_index;

                uint32_t last_node_id = 0;
                const char* last_tag = "";

                int gate_input_count = 1;

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

                    if (strcmp(tag_name, "assign.lhs") == 0) {
                        TSNode nameNode = capture.node;
                        sn->identifier = code.substr(ts_node_start_byte(nameNode),
                            ts_node_end_byte(nameNode) - ts_node_start_byte(nameNode));
                        Port p;
                        // identifier 存槽位名: In1, In2...
                        p.identifier = "Out1";

                        // direction 设为输入
                        p.direction = PortDirection::Out;

                        // conn 存实际连接的信号名
                        p.conn = code.substr(ts_node_start_byte(nameNode),
                            ts_node_end_byte(nameNode) - ts_node_start_byte(nameNode));

                        sn->ports.push_back(p);

                    }
                    else if (strcmp(tag_name, "assign.rhs") == 0) {
                        TSNode nameNode = capture.node;
                        std::set<std::string> rhsSignals;
                        int input_count = 1;
                        ExpressionResult res = FormalizeExpression(capture.node, code, input_count, -1);
                        sn->assign_expression = res.template_text;
                        sn->ports.insert(sn->ports.end(), res.extracted_ports.begin(), res.extracted_ports.end());
                    }
                }

                }
            sn->parent = newParent;
            CollectSigTreeNodeInfoTS(sn, currentNode, filePath, code);
            newParent->children.push_back(sn);
            newParent = sn;
        }
    }

    if (ts_tree_cursor_goto_first_child(cursor)) {
        // 递归子节点
        do {
            UpdateTreeFromTS(cursor, newParent, filePath, code);
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
    else if(node_type == "gate_instantiation") return SecondNodeType::GateInstance;
    else if(node_type == "always_construct") return SecondNodeType::Always;
    else if(node_type == "continuous_assign") return SecondNodeType::ContinuousAssign;
    else return SecondNodeType::Null;
}

bool isTSNodeNet(std::string node_type) {
    if (node_type == "net_declaration") return true;
    else return false;
}

void CollectSigTreeNodeInfoTS(SigTreeNode* node, TSNode& TSnode, std::string filepath, std::string code) {
    // Verilog Info
    node->verilogInfo.filePath = filepath;

    TSPoint start_pt = ts_node_start_point(TSnode);
    node->verilogInfo.startLine = start_pt.row + 1;
    node->verilogInfo.startCol = start_pt.column + 1;
    TSPoint end_pt = ts_node_end_point(TSnode);
    node->verilogInfo.endLine = end_pt.row + 1;
    node->verilogInfo.endCol = end_pt.column + 1;

    node->verilogInfo.Code = code.substr(ts_node_start_byte(TSnode),
        ts_node_end_byte(TSnode) - ts_node_start_byte(TSnode));
    

    // Schematic Info

}

void SigFlowTree::ConstructDefinitionTable() {
    for (SigTreeNode* sig : root->children) {
        for (SigTreeNode* s : sig->children) {
            TopNode* def = static_cast<TopNode*>(s);
            DefinitionTable.emplace(def->identifier, def);
        }
    }
}

void SigFlowTree::ConstructInstanceTable() {
    for (SigTreeNode* sig : root->children) {
        for (SigTreeNode* si : sig->children) {
            for (SigTreeNode* s : si->children) {
                SecondNode* inst = static_cast<SecondNode*>(s);
                if (inst->secondType == SecondNodeType::ModuleInstance || inst->secondType == SecondNodeType::UDPInstance) {
                    InstanceTable.emplace(inst->identifier, inst);
                }
            }

        }
    }
}

void SigFlowTree::LinkInstsWithDefs() {
    // 1. 遍历所有的实例节点
    for (auto& pair : InstanceTable) {
        LinkSingleInstWithDef(pair.second);
    }
}

void SigFlowTree::LinkSingleInstWithDef(SecondNode* inst) {
    if (!inst) return;

    // 1. 查找定义
    auto it = DefinitionTable.find(inst->defIdentifier);

    if (it != DefinitionTable.end()) {
        TopNode* def = it->second;
        inst->Definition = def;

        // 2. 同步端口属性（方向等）
        // 这一步是确保实例的行为与其定义的模板一致
        for (auto& instPort : inst->ports) {
            auto defPortIt = std::find_if(def->ports.begin(), def->ports.end(),
                [&instPort](const Port& p) {
                    return p.identifier == instPort.identifier;
                });

            if (defPortIt != def->ports.end()) {
                // 同步来自定义的关键元数据
                instPort.direction = defPortIt->direction;
            }
            // 如果没找到，instPort.direction 保持默认或之前状态
        }
    }
    else {
        // 找不到定义，置为空以防野指针
        HangInst(inst);
    }
}

void SigFlowTree::HangInst(SecondNode* inst) {
    // 保持defIdentifier，用于寻找可能的引用
    inst->Definition = nullptr;
    for (auto& port : inst->ports) {

        port.direction = PortDirection::InOut;
    }
}


void SigFlowTree::UpdateTreeFromSlang(slang::ast::Compilation* compilation) {

}




void SigFlowTree::PrintTree() {
    OutputDebugStringA("######################## SigTree ########################\n");

    root->PrintProjectNode();
    OutputDebugStringA("\n");

    for (SigTreeNode* node : root->children) {
        FileNode* fn = static_cast<FileNode*>(node);
        fn->PrintFileNode();
        OutputDebugStringA("\n");
        for (SigTreeNode* child : fn->children) {
            TopNode* tn = static_cast<TopNode*>(child);
            tn->PrintTopNode();
            OutputDebugStringA("\n");
            for (SigTreeNode* child2 : tn->children) {
                if (child2->type == SigTreeNodeType::Second) {
                    SecondNode* sn = static_cast<SecondNode*>(child2);
                    sn->PrintSecondNode();
                    OutputDebugStringA("\n");
                }
                else if (child2->type == SigTreeNodeType::Signal) {
                    SignalNode* nn = static_cast<SignalNode*>(child2);
                    nn->PrintSignalNode();
                    OutputDebugStringA("\n");
                }

            }
        }

    };
}
void SigTreeNode::PrintSigTreeNode() {
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

    info += "Verilog Info: \n";
    info += std::format("{} ({},{})-({},{})\n", verilogInfo.filePath, verilogInfo.startLine, verilogInfo.startCol, verilogInfo.endLine, verilogInfo.endCol);
    info += "code: " + verilogInfo.Code.substr(0, 30) + "\n";

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

std::string ProjectNode::ToVerilog(){
    return std::format("");
}

std::string FileNode::ToVerilog() {
    std::string v;
    for (auto* child : children) {
        v += child->ToVerilog();
    }

    return v;
}


std::string TopNode::ToVerilog() {
    std::string v = std::format("module {}", identifier);
    if (!ports.empty()) {
        v += "(";
        for (auto port : ports) {
            v += std::format("{} {},\n", port.portDirectionToStr(port.direction), port.identifier);

        }
        v += ")\n";
    }

    for (auto* child : children) {
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

std::string SecondNode::ToVerilog() {
    switch (secondType) {
    case SecondNodeType::ModuleInstance:
        return ModuleInstanceToVerilog();
    case SecondNodeType::ContinuousAssign:
        return ContinuousAssignToVerilog();
    case SecondNodeType::GateInstance:
        return GateInstanceToVerilog();
    default:
        return "";
    }
}

std::string SecondNode::ModuleInstanceToVerilog() {
    std::string v = std::format("{} ", identifier);
    if (!ports.empty()) {
        v += "(";
        for (auto port : ports) {
            v += std::format(".{}({}),\n", port.identifier, port.conn);
        }
        v += ")\n";
    }
    return v;
}

std::string SecondNode::ContinuousAssignToVerilog() {
    std::string v = std::format("assign ");
    for (auto port : ports) {
        if (port.direction == PortDirection::Out) {
            v += port.conn + " = ";
            break;
        }
    }
    for (auto port : ports) {
        if (port.direction == PortDirection::In) {
            v += port.conn + " _ ";
        }
    }
    v += ";\n";
    return v;
}

std::string SecondNode::GateInstanceToVerilog() {
    std::string v = gatetype + " " + identifier + " (";
    for (auto port : ports) {
        if (port.direction == PortDirection::Out) {
            v += port.conn + ", ";
            break;
        }

    }
    for (auto port : ports) {
        if (port.direction == PortDirection::In) {
            v += port.conn + ", ";
        }
    }
    v += ");\n";
    return v;
}
void SecondNode::DelExpressionsPort(int del) {
    if (del < 0 || del >= (int)ports.size()) return;

    // 1. 先删除实际的元素
    ports.erase(ports.begin() + del);

    // 2. 更新所有表达式中的索引映射
    for (NB_OR_B_Expression& exp : nb_or_b_expressions) {
        // 更新输出端口索引
        if (exp.out_port_id == del) {
            exp.out_port_id = -1; // 或者指向一个默认值，表示原引用已消失
        }
        else if (exp.out_port_id > del) {
            exp.out_port_id--;
        }

        // 更新输入端口索引列表
        for (auto it = exp.in_port_ids.begin(); it != exp.in_port_ids.end(); ) {
            if (*it == del) {
                it = exp.in_port_ids.erase(it); // 如果引用的正是被删掉的，从表达式中移除引用
            }
            else {
                if (*it > del) (*it)--;
                ++it;
            }
        }
    }
}

void SecondNode::AddExpressionsPort(int exp_id) {
    if (exp_id < 0 || exp_id >= (int)nb_or_b_expressions.size()) return;

    NB_OR_B_Expression& exp = nb_or_b_expressions[exp_id];

    // 1. 准备新端口数据
    Port newPort;
    newPort.identifier = "In" + std::to_string(exp_id+1) + "_" + std::to_string(exp.in_port_ids.size()+1);
    newPort.direction = PortDirection::In;

    // 2. 确定插入位置
    // 如果该表达式已有输入端口，插在最后一个输入端口后面；否则插在输出端口后面
    int insertPos;
    if (!exp.in_port_ids.empty()) {
        insertPos = exp.in_port_ids.back() + 1;
    }
    else {
        insertPos = exp.out_port_id + 1;
    }

    // 边界安全检查
    if (insertPos > (int)ports.size()) insertPos = (int)ports.size();

    // 3. 全局索引偏移处理
    // 凡是等于或大于插入位置的索引，全部加 1，为新成员腾出位置
    for (NB_OR_B_Expression& e : nb_or_b_expressions) {
        if (e.out_port_id >= insertPos) e.out_port_id++;
        for (int& id : e.in_port_ids) {
            if (id >= insertPos) id++;
        }
    }

    // 4. 执行物理插入
    ports.insert(ports.begin() + insertPos, newPort);

    // 5. 将新索引关联到当前表达式
    exp.in_port_ids.push_back(insertPos);
}

void SecondNode::AddEmptyExpression() {
    NB_OR_B_Expression newExp;
    newExp.nb_or_b_expression = ""; // 默认右值
    newExp.is_blocking = false;              // 默认非阻塞 <=
    newExp.delay = 0.0f;

    // 创建一个对应的默认输出端口
    Port outP;
    outP.identifier = "Out" + std::to_string(nb_or_b_expressions.size()+1);
    outP.direction = PortDirection::Out;

    // 使用你之前的逻辑：先加 Port，再记索引
    newExp.out_port_id = (int)this->ports.size();
    this->ports.push_back(outP);

    this->nb_or_b_expressions.push_back(newExp);
}

void SecondNode::DelLastExpression() {
    if (nb_or_b_expressions.empty()) return;

    // 可选：你可能想同时删除该表达式关联的所有 ports
    // 这里简单处理：只弹出最后一个表达式
    nb_or_b_expressions.pop_back();
}


std::string ProjectNode::GetDisplayName() { return "Project " + std::filesystem::path(projectPath).filename().string(); }
std::string FileNode::GetDisplayName()  { return "File " + std::filesystem::path(filePath).filename().string(); }
std::string TopNode::GetDisplayName() { return SigFlowTree::ToString(topType) + " " + identifier; }
std::string SecondNode::GetDisplayName() {
    switch (secondType) {
    case SecondNodeType::ModuleInstance:
        return defIdentifier + " " + identifier;
        break;
    case SecondNodeType::GateInstance:
        return gatetype + " " + identifier;
        break;
    case SecondNodeType::ContinuousAssign:
        return SigFlowTree::ToString(secondType) + " " + ports[0].conn;
        break;
    default:
        return SigFlowTree::ToString(secondType) + " " + identifier;
        break;
    }
};

std::string SignalNode::GetDisplayName()  { return SigFlowTree::ToString(signalType) + " " + identifier; }




void ProjectNode::PrintProjectNode() {
    SigTreeNode::PrintSigTreeNode();
    std::string info;

    info = "ProjectPath: "+ projectPath + "\n";
    OutputDebugStringA(info.c_str());
}

void FileNode::PrintFileNode() {
    SigTreeNode::PrintSigTreeNode();
    std::string info;

    info = "FilePath: " + filePath + "\n";
    OutputDebugStringA(info.c_str());
}

void TopNode::PrintTopNode() {
    SigTreeNode::PrintSigTreeNode();
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

    for (Port& p : ports) {
        info += "port: " + p.identifier + " " + std::string(p.direction == PortDirection::In ? "In" : "Out") + "\n";
    }


    OutputDebugStringA(info.c_str());
}

void SecondNode::PrintSecondNode() {
    SigTreeNode::PrintSigTreeNode();
    std::string info;
    info = "definition: " + defIdentifier + "\n";
    info = "Identifier: " + identifier + "\n";

    switch (secondType) {
    case SecondNodeType::ModuleInstance:
        info += "SecondType: ModuleInstance\n";
        info += "Definition: " + defIdentifier + "\n";
        break;
    case SecondNodeType::Always:
        info += "SecondType: Always\n";
        for (int i = 0; i < nb_or_b_expressions.size(); i++) {
            info += std::format("#{} {} {} {}\n", nb_or_b_expressions[i].delay, ports[nb_or_b_expressions[i].out_port_id].conn, nb_or_b_expressions[i].is_blocking?"=":"<=", nb_or_b_expressions[i].nb_or_b_expression);
            info += std::format("inPorts ids:");
            for (int id : nb_or_b_expressions[i].in_port_ids) {
                info += std::format("{} ", id);
            }
            info += std::format("\n");
        }
        



        break;
    case SecondNodeType::ContinuousAssign:
        info += "SecondType: ContinuousAssign\n";
        info += "Expression: " + assign_expression + "\n";
        break;
    case SecondNodeType::GateInstance:
        info += "SecondType: GateInstance\n";
        break;
    default:
        info += "SecondType: Unknown\n";
    }


    for (Port& p : ports) {

        std::string pd;
        switch (p.direction) {
        case PortDirection::In:
            pd = "In";
            break;
        case PortDirection::Out:
            pd = "Out";
            break;
        case PortDirection::InOut:
            pd = "InOut";
            break;
        case PortDirection::Ref:
            pd = "Ref";
            break;
        }
        info += "port: " + p.identifier + " " + pd + " to: " + p.conn + "\n";
    }

    OutputDebugStringA(info.c_str());

}

void SignalNode::PrintSignalNode() {
    SigTreeNode::PrintSigTreeNode();
    std::string info;
    info += "Signal Type: " + SigFlowTree::ToString(signalType) + "\n";
    info += "Identifier: " + identifier + "\n";

    OutputDebugStringA(info.c_str());

}

void SigTreeNode::RemoveChild(SigTreeNode* child) {
    auto it = std::find(children.begin(), children.end(), child);
    if (it == children.end())
        return;

    (*it)->parent = nullptr;
    children.erase(it);
}
void SigTreeNode::AddChild(SigTreeNode* child) {
    if (!child) return;

    // 防止重复插入
    if (child->parent == this) return;

    // 如果之前有父节点，先从原父节点解绑
    if (child->parent) {
        child->parent->RemoveChild(child);
    }

    child->parent = this;
    children.push_back(child);
}




void SigFlowTree::ClearTree() {
    DefinitionTable.clear();
    InstanceTable.clear();
    SignalTable.clear();
    root = nullptr;
    arena.reset(); // 释放所有内存块
}

void SigFlowTree::RemoveChild(SigTreeNode* parent, SigTreeNode* child) {
    if (!parent || !child) return;

    UnregisterNodeRecursive(child);
    parent->RemoveChild(child);
    
}

SigTreeNode* SigFlowTree::AddChild(SigTreeNode* parent, SigTreeNode* externalNode) {
    if (!externalNode) return nullptr;

    // A. 深度克隆：把外部数据搬进自己的 Arena 领地
    SigTreeNode* safeNode = CloneSubtreeToArena(externalNode);

    // B. 建立物理父子关系
    if (parent) {
        parent->AddChild(safeNode); // 使用 SigTreeNode::AddChild 处理指针
    }
    else {
        // 如果是 ProjectNode 或根节点手动指定
        if (safeNode->type == SigTreeNodeType::Project) {
            this->root = static_cast<ProjectNode*>(safeNode);
        }
    }

    // C. 事务处理：填表并自动完成链接（Link）
    RegisterNodeRecursive(safeNode);
    return safeNode;
}

SigTreeNode* SigFlowTree::CloneSubtreeToArena(SigTreeNode* node) {
    if (!node) return nullptr;

    // 利用子类实现的 Clone(arena) 方法进行多态拷贝
    SigTreeNode* newNode = node->Clone(this->arena);

    // 递归克隆所有子节点
    for (auto child : node->children) {
        SigTreeNode* newChild = CloneSubtreeToArena(child);
        newNode->children.push_back(newChild);
        newChild->parent = newNode;
    }
    return newNode;
}

void SigFlowTree::RegisterNodeRecursive(SigTreeNode* node) {
    if (!node) return;

    // 根据类型重新填表
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
            InstanceTable[sn->identifier] = sn;
            LinkSingleInstWithDef(sn);
        }
    }

    for (auto child : node->children) {
        RegisterNodeRecursive(child);
    }
}

void SigFlowTree::UnregisterNodeRecursive(SigTreeNode* node) {
    if (!node) return;

    // 1. 根据当前节点类型，从对应的表中移除
    switch (node->type) {
    case SigTreeNodeType::Top: {
        TopNode* top = static_cast<TopNode*>(node);
        DefinitionTable.erase(top->identifier);

        // 可选：断开所有引用该定义的实例链接
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
        // 如果你有 SignalTable，在这里 erase
        break;
    }
    default: break;
    }

    // 2. 关键：递归处理所有子节点
    // 确保整个子树里的所有符号都被从 Map 中清理干净
    for (auto child : node->children) {
        UnregisterNodeRecursive(child);
    }
}

FileNode* SigFlowTree::GetFileNode(std::string filePath) {
    for (SigTreeNode* cld : root->children) {
        FileNode* fn = static_cast<FileNode*>(cld);
        if (fn->filePath == filePath) return fn;
    }
    return nullptr;
}


std::vector<int> SigFlowTree::SecondNodeTopoLevel(TopNode* tn)
{
    if (!tn) return {};

    // 1️⃣ 收集 SecondNode
    std::vector<SecondNode*> nodes;
    for (auto* child : tn->children)
    {
        if (child->type == SigTreeNodeType::Second)
            nodes.push_back(static_cast<SecondNode*>(child));
    }

    int n = nodes.size();
    std::vector<int> indegree(n, 0);
    std::vector<int> level(n, 0);

    // 建立索引映射
    std::unordered_map<SecondNode*, int> index;
    for (int i = 0; i < n; ++i)
        index[nodes[i]] = i;

    // 2️⃣ 构建图（邻接表）
    std::vector<std::vector<int>> adj(n);

    for (int i = 0; i < n; ++i)
    {
        for (int j = 0; j < n; ++j)
        {
            if (i == j) continue;

            // i.out → j.in ?
            for (auto& p_out : nodes[i]->ports)
            {
                if (p_out.direction != PortDirection::Out)
                    continue;

                for (auto& p_in : nodes[j]->ports)
                {
                    if (p_in.direction != PortDirection::In)
                        continue;

                    if (!p_out.conn.empty() && p_out.conn == p_in.conn)
                    {
                        adj[i].push_back(j);
                        indegree[j]++;
                    }
                }
            }
        }
    }

    // 3️⃣ Kahn 拓扑分层
    std::queue<int> q;

    for (int i = 0; i < n; ++i)
    {
        if (indegree[i] == 0)
        {
            level[i] = 0;
            q.push(i);
        }
    }

    int processed = 0;

    while (!q.empty())
    {
        int u = q.front();
        q.pop();
        processed++;

        for (int v : adj[u])
        {
            level[v] = std::max(level[v], level[u] + 1);
            indegree[v]--;

            if (indegree[v] == 0)
                q.push(v);
        }
    }

    // 4️⃣ 处理环（剩余 indegree > 0 的节点）
    if (processed < n)
    {
        int maxLevel = 0;
        for (int i = 0; i < n; ++i)
            maxLevel = std::max(maxLevel, level[i]);

        for (int i = 0; i < n; ++i)
        {
            if (indegree[i] > 0)
            {
                // 将环内节点设为同一层
                level[i] = maxLevel;
            }
        }
    }

    return level;
}
