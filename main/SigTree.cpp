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

extern "C" TSLanguage* tree_sitter_verilog();

namespace fs = std::filesystem;

TopNodeType isTSNodeTop(std::string node_type);
SecondNodeType isTSNodeSecond(std::string node_type);
bool isTSNodeNet(std::string node_type);
void CollectSigTreeNodeInfoTS(SigTreeNode* node, TSNode& TSnode, std::string filepath, std::string code);

SigFlowTree::SigFlowTree(MainFrame* parent): m_parent(parent) {
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
            placeholder = "in" + std::to_string(exp_id) + "_" + std::to_string(in_counter++);
        }
        else {
            placeholder = "in" + std::to_string(in_counter++);
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
            fn = static_cast<FileNode*>(this->AddChild(newParent, fn));
            //CollectSigTreeNodeInfoTS(fn, currentNode, filePath, code);
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
                        else if(p.direction == PortDirection::Out) out.push_back(p);
                    }
                }
            }
            ts_query_cursor_delete(cursor);

            TopNode* tn = arena.make<TopNode>(id, top_type);
            tn->UpdateInPorts(in);
            tn->UpdateOutPorts(out);
            tn = static_cast<TopNode*>(this->AddChild(newParent, tn));
            //CollectSigTreeNodeInfoTS(tn, currentNode, filePath, code);
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

                        // 3. 识别零件的标签名（通过 ID 换取字符串）
                        uint32_t name_len;
                        const char* tag_name = ts_query_capture_name_for_id(second, capture.index, &name_len);

                        // 4. 根据标签名分发逻辑
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
                //ModuleInstNode* mn = arena.make<ModuleInstNode>(id, def);
                //mn->inout_ports = ps;
                mn = static_cast<ModuleInstNode*>(this->AddChild(newParent, mn));
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
                newParent = gn;
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
                EdgeType et;
                std::string id;
                std::vector<Port> in;
                std::vector<Port> out;
                std::vector<NB_OR_B_Expression> exps;

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
                            et = EdgeType::Posedge;
                        }
                        else if (edgeType == "negedge") {
                            et = EdgeType::Negedge;
                        }

                    }
                    else if (strcmp(tag_name, "clk.name") == 0) {
                        TSNode nameNode = capture.node;
                        Port p("CLK", PortDirection::In, code.substr(ts_node_start_byte(nameNode),
                            ts_node_end_byte(nameNode) - ts_node_start_byte(nameNode)));

                        in.push_back(p);
                        id = "@" + p.conn;

                    }
                    else if (strcmp(tag_name, "assign.lhs") == 0) {
                        TSNode nameNode = capture.node;
                        Port p(std::format("out{}", onput_count++), PortDirection::Out, code.substr(ts_node_start_byte(nameNode),
                            ts_node_end_byte(nameNode) - ts_node_start_byte(nameNode)));

                        out.push_back(p);
                        exp.out_port_id = out.size() - 1;


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
                        int startIndex = (int)in.size();
                        in.insert(in.end(), res.extracted_ports.begin(), res.extracted_ports.end());
                        int endIndex = (int)in.size() ;
                        exp.in_port_ids.clear();
                        for (int i = startIndex; i < endIndex; ++i) {
                            exp.in_port_ids.push_back(i);
                        }
                        exps.push_back(exp);
                        exp_input_count++;

                    }
                }
                AlwaysNode* an = arena.make<AlwaysNode>(id, et);
                an->in_ports = in;
                an->out_ports = out;
                an->nb_or_b_expressions = exps;
                an = static_cast<AlwaysNode*>(this->AddChild(newParent, an));
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
                        continue; // 同样的标签在同样的位置，跳过！
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
                        std::set<std::string> rhsSignals;
                        int input_count = 1;
                        ExpressionResult res = FormalizeExpression(capture.node, code, input_count, -1);
                        exp = res.template_text;
                        in.insert(in.end(), res.extracted_ports.begin(), res.extracted_ports.end());
                    }
                }
                ContinuousAssignNode* an = arena.make<ContinuousAssignNode>(id, exp);
                an->in_ports = in;
                an->out_ports.push_back(out);
                an = static_cast<ContinuousAssignNode*>(this->AddChild(newParent, an));
                newParent = an;


                }
            //CollectSigTreeNodeInfoTS(sn, currentNode, filePath, code);
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

/*

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

}*/

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

    // 预分配内存以提高性能（可选，但推荐）
    defs.reserve(DefinitionTable.size());

    // 遍历 Map，提取所有的键（Definition Name）
    for (auto const& [name, node] : DefinitionTable) {
        defs.push_back(name);
    }

    return defs;
}

void SigFlowTree::LinkInstsWithDefs() {
    // 1. 遍历所有的实例节点
    for (auto& pair : InstanceTable) {
        LinkSingleInstWithDef(pair.second);
    }
}

void SigFlowTree::LinkSingleInstWithDef(ModuleInstNode* inst) {
    HangInst(inst);
    if (!inst) return;
    inst->in_ports.clear();
    inst->out_ports.clear();
    // 1. 查找定义
    auto it = DefinitionTable.find(inst->defIdentifier);

    if (it != DefinitionTable.end()) {
        TopNode* def = it->second;
        inst->Definition = def;

        // 2. 同步端口属性（方向等）
        // 这一步是确保实例的行为与其定义的模板一致
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
                    // 同步来自定义的关键元数据
                    instPort.direction = defInPortIt->direction;
                    inst->in_ports.push_back(instPort);
                }
                else if (defOutPortIt != def->GetOutPorts().end()) {
                    instPort.direction = defOutPortIt->direction;
                    inst->out_ports.push_back(instPort);

                }

                // 如果没找到，instPort.direction 保持默认或之前状态
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
    else {
        // 找不到定义，置为空以防野指针
    }
}

void SigFlowTree::HangInst(ModuleInstNode* inst) {
    // 保持defIdentifier，用于寻找可能的引用
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

    /*
    info += "Verilog Info: \n";
    info += std::format("{} ({},{})-({},{})\n", verilogInfo.filePath, verilogInfo.startLine, verilogInfo.startCol, verilogInfo.endLine, verilogInfo.endCol);
    info += "code: " + verilogInfo.Code.substr(0, 30) + "\n";*/

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


void AlwaysNode::DelExpressionsPort(int idx, int del) {
    
    if (del < 0 || del >= (int)in_ports.size()) return;

    // 1. 先删除实际的元素
    in_ports.erase(in_ports.begin() + del);

    // 2. 更新所有表达式中的索引映射
    NB_OR_B_Expression& exp = nb_or_b_expressions[idx];

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

void AlwaysNode::DelExpressionsPort(int del) {

    if (del < 0 || del >= (int)in_ports.size()) return;

    // 1. 先删除实际的元素
    in_ports.erase(in_ports.begin() + del);

    // 2. 更新所有表达式中的索引映射
    for (NB_OR_B_Expression& exp : nb_or_b_expressions) {
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



void AlwaysNode::AddExpressionsPort(int exp_id) {
    
    if (exp_id < 0 || exp_id >= (int)nb_or_b_expressions.size()) return;

    NB_OR_B_Expression& exp = nb_or_b_expressions[exp_id];

    // 1. 准备新端口数据
    Port newPort;
    newPort.identifier = "in" + std::to_string(exp_id+1) + "_" + std::to_string(exp.in_port_ids.size()+1);
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
    if (insertPos > (int)in_ports.size()) insertPos = (int)in_ports.size();

    // 3. 全局索引偏移处理
    // 凡是等于或大于插入位置的索引，全部加 1，为新成员腾出位置
    for (NB_OR_B_Expression& e : nb_or_b_expressions) {
        if (e.out_port_id >= insertPos) e.out_port_id++;
        for (int& id : e.in_port_ids) {
            if (id >= insertPos) id++;
        }
    }

    // 4. 执行物理插入
    in_ports.insert(in_ports.begin() + insertPos, newPort);

    // 5. 将新索引关联到当前表达式
    exp.in_port_ids.push_back(insertPos);
}

void AlwaysNode::AddEmptyExpression() {
    
    NB_OR_B_Expression newExp;
    newExp.nb_or_b_expression = ""; // 默认右值
    newExp.is_blocking = false;              // 默认非阻塞 <=
    newExp.delay = 0.0f;

    // 创建一个对应的默认输出端口
    Port outP;
    outP.identifier = "out" + std::to_string(nb_or_b_expressions.size()+1);
    outP.direction = PortDirection::Out;

    // 使用你之前的逻辑：先加 Port，再记索引
    newExp.out_port_id = (int)this->out_ports.size();
    this->out_ports.push_back(outP);

    this->nb_or_b_expressions.push_back(newExp);
}

void AlwaysNode::DelLastExpression() {
    if (nb_or_b_expressions.empty()) return;

    // 可选：你可能想同时删除该表达式关联的所有 ports
    // 这里简单处理：只弹出最后一个表达式
    nb_or_b_expressions.pop_back();
}


std::string ProjectNode::GetName() { return "Project " + std::filesystem::path(projectPath).filename().string(); }
std::string FileNode::GetName()  { return "File " + std::filesystem::path(filePath).filename().string(); }
std::string TopNode::GetName() { return SigFlowTree::ToString(topType) + " " + identifier; }
std::string SecondNode::GetName() { return SigFlowTree::ToString(secondType) + " " + identifier;};
std::string SignalNode::GetName()  { return SigFlowTree::ToString(signalType) + " " + identifier; }




void ProjectNode::Print() {
    SigTreeNode::Print();
    std::string info;

    info = "ProjectPath: "+ projectPath + "\n";
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
    for (Port& p : in_ports) {
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

    //(*it)->parent = nullptr;
    children.erase(it);
}

bool SigTreeNode::AddChild(SigTreeNode* child) {
    if (!child) return false;

    if (this->CanBeChild(child->type) && child->CanBeParent(this->type)) {

        // 防止重复插入
        if (child->parent == this) return false;

        // 如果之前有父节点，先从原父节点解绑
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
    arena.reset(); // 释放所有内存块
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

    // A. 契约预检 (最重要的一步！)
    if (parent) {
        // 使用虛函数多态检查，无需克隆即可判断是否合法
        if (!parent->CanBeChild(externalNode->type) ||
            !externalNode->CanBeParent(parent->type)) {
            // 这里可以记录错误日志：无法将 externalNode 挂载到 parent 下
            return nullptr; // 拒绝克隆，没有任何内存浪费
        }
    }
    else {
        if (externalNode->type != SigTreeNodeType::Project) {
            return nullptr; // 根节点必须是 Project
        }
    }

    // B. 安全的深度克隆
    SigTreeNode* safeNode = CloneSubtreeToArena(externalNode);

    // C. 物理挂载 (此时 AddChild 应当总能成功)
    if (parent) {
        parent->AddChild(safeNode);
        wxCommandEvent event(EVT_SIGFLOWNODE_ADD);
        event.SetClientData(safeNode);
        wxPostEvent(m_parent->GetEventHandler(), event);
    }
    else {
        this->root = static_cast<ProjectNode*>(safeNode);
    }

    // D. 业务逻辑注册
    RegisterNodeRecursive(safeNode);
    return safeNode;
}

SigTreeNode* SigFlowTree::CloneSubtreeToArena(SigTreeNode* node) {
    if (!node) return nullptr;

    // 利用子类实现的 Clone(arena) 方法进行多态拷贝
    SigTreeNode* newNode = node->Clone(this->arena);

    // 递归克隆所有子节点
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
    /*
    if (p.direction == PortDirection::InOut)
        sn->inout_ports.push_back(p);
    wxCommandEvent evt(EVT_SIGFLOWNODE_CHANGED);
    evt.SetClientData(sn);
    wxPostEvent(m_parent->GetEventHandler(), evt);*/

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
            //wxPostEvent(m_parent->GetEventHandler(), evt);
            m_parent->GetEventHandler()->ProcessEvent(evt);
        }
    }
    else if(p.direction == PortDirection::Out) {
        auto it = std::find_if(sn->GetOutPorts().begin(), sn->GetOutPorts().end(),
            [&](const Port& item) {
                return item.identifier == p.identifier;
            });
        if (it != sn->GetOutPorts().end()) {
            sn->GetOutPorts().erase(it);
            wxCommandEvent evt(EVT_SIGFLOWNODE_CHANGED);
            evt.SetClientData(sn);
            //wxPostEvent(m_parent->GetEventHandler(), evt);
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
        //wxPostEvent(m_parent->GetEventHandler(), evt);
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
        //wxPostEvent(m_parent->GetEventHandler(), evt);
        m_parent->GetEventHandler()->ProcessEvent(evt);
    }
    
}


void SigFlowTree::PortReName(TopNode* tn, wxString old_id, wxString new_id) {
    // 1. 查找旧端口（先找输入，再找输出）
    auto findAndRename = [&](std::vector<Port>& ports) -> bool {
        auto it = std::find_if(ports.begin(), ports.end(),
            [&](const Port& item) { return item.identifier == old_id; });

        if (it != ports.end()) {
            // 执行重命名核心逻辑
            it->identifier = new_id;
            return true;
        }
        return false;
        };

    bool changed = findAndRename(tn->GetInPorts());
    if (!changed) {
        changed = findAndRename(tn->GetOutPorts());
    }

    // 2. 如果发生了修改，通知 UI
    if (changed) {
        wxCommandEvent* evt = new wxCommandEvent(EVT_SIGFLOWNODE_CHANGED);
        evt->SetClientData(tn);

        // 核心：使用 QueueEvent 或 wxPostEvent 异步通知
        // 避免在重命名瞬间（通常是输入框失去焦点时）立即销毁/重建 UI 导致崩溃
        m_parent->GetEventHandler()->QueueEvent(evt);
    }
}


void SigFlowTree::PortReName(SecondNode* sn, wxString old_id, wxString new_id) {
    // 1. 查找旧端口（先找输入，再找输出）
    auto findAndRename = [&](std::vector<Port>& ports) -> bool {
        auto it = std::find_if(ports.begin(), ports.end(),
            [&](const Port& item) { return item.identifier == old_id; });

        if (it != ports.end()) {
            // 执行重命名核心逻辑
            it->identifier = new_id;
            return true;
        }
        return false;
        };

    bool changed = findAndRename(sn->in_ports);
    if (!changed) {
        changed = findAndRename(sn->out_ports);
    }

    // 2. 如果发生了修改，通知 UI
    if (changed) {
        wxCommandEvent* evt = new wxCommandEvent(EVT_SIGFLOWNODE_CHANGED);
        evt->SetClientData(sn);

        // 核心：使用 QueueEvent 或 wxPostEvent 异步通知
        // 避免在重命名瞬间（通常是输入框失去焦点时）立即销毁/重建 UI 导致崩溃
        m_parent->GetEventHandler()->QueueEvent(evt);
    }
}

void SigFlowTree::PortConn(SecondNode* sn, wxString id, wxString conn) {
    if (!sn) return;

    bool found = false;

    // 1. 查找并修改 InPorts
    for (auto& p : sn->in_ports) {
        if (p.identifier == id) {
            p.conn = conn; // 假设 Port 结构体有 conn 成员
            found = true;
            break;
        }
    }

    // 2. 如果没找到，查找并修改 OutPorts
    if (!found) {
        for (auto& p : sn->out_ports) {
            if (p.identifier == id) {
                p.conn = conn;
                found = true;
                break;
            }
        }
    }

    // 3. 上报事件，通知 UI 更新 (使用异步事件保证安全)
    if (found) {
        wxCommandEvent* evt = new wxCommandEvent(EVT_SIGFLOWNODE_CHANGED);
        evt->SetClientData(sn); // 携带节点信息
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


std::vector<int> SigFlowTree::SecondNodeTopoLevel(TopNode* tn)
{
    if (!tn) return {};

    // 1️⃣ 收集 SecondNode
    std::vector<SecondNode*> nodes;
    for (auto* child : tn->GetChildren())
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
            for (auto& p_out : nodes[i]->out_ports)
            {

                for (auto& p_in : nodes[j]->in_ports)
                {

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


GateInstNode::GateInstNode(std::string id, GateType gt) :SecondNode(id, SecondNodeType::GateInstance), gatetype(gt) {
    in_ports.push_back(Port("in1", PortDirection::In));
    in_ports.push_back(Port("in2", PortDirection::In));
    out_ports.push_back(Port("out", PortDirection::Out));
};

std::string GateInstNode::ToVerilog() {
    std::string v = SigFlowTree::ToString(gatetype) + " " + identifier + " (";
    for (auto port : out_ports) {
        v += port.conn + " = ";
    }
    for (auto port : in_ports) {
        v += port.conn + " = ";
    }
    v += ");\n";
    return v;
}

std::string GateInstNode::GetName() {
    return SigFlowTree::ToString(gatetype) + " " + identifier;
}

void GateInstNode::Print() {
    OutputDebugStringA("SecondType: GateInstance\n");
    SecondNode::Print();
}

ModuleInstNode::ModuleInstNode(std::string id, TopNode* Definition) :SecondNode(id, SecondNodeType::ModuleInstance) {
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
    std::string v = std::format("{} ", identifier);
    v += "(";
    for (auto port : in_ports) {
        v += std::format(".{}({}),\n", port.identifier, port.conn);
    }
    for (auto port : out_ports) {
        v += std::format(".{}({}),\n", port.identifier, port.conn);
    }
    v += ")\n";

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


ContinuousAssignNode::ContinuousAssignNode(std::string id, std::string raw_assign) :SecondNode(id, SecondNodeType::ContinuousAssign){
    /*
    std::string code = std::format("module _tmp\n{} = {};\n endmodule;", id, raw_assign);
    TSParser* parser = ts_parser_new();
    ts_parser_set_language(parser, tree_sitter_verilog());
    TSTree* tree = ts_parser_parse_string(parser, nullptr, code.c_str(), code.length());





    ts_parser_delete(parser);
    ts_tree_delete(tree);*/
};

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


std::string AlwaysNode::ToVerilog() {
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

std::string AlwaysNode::GetName() {
    return SigFlowTree::ToString(secondType) + " " + identifier;
}

void AlwaysNode::Print() {
    std::string info;
    info += "SecondType: Always\n";
    for (int i = 0; i < nb_or_b_expressions.size(); i++) {
        //info += std::format("#{} {} {} {}\n", nb_or_b_expressions[i].delay, ports[nb_or_b_expressions[i].out_port_id].conn, nb_or_b_expressions[i].is_blocking?"=":"<=", nb_or_b_expressions[i].nb_or_b_expression);
        info += std::format("inPorts ids:");
        for (int id : nb_or_b_expressions[i].in_port_ids) {
            info += std::format("{} ", id);
        }
        info += std::format("\n");
    }
    OutputDebugStringA(info.c_str());
    SecondNode::Print();
}
