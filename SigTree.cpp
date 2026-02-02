#include "SigTree.h"

#include <json/json.h>
#include <iostream>
#include <fstream>
#include <filesystem>
#include <memory>
#include <windows.h>
#include <regex>

extern "C" TSLanguage* tree_sitter_verilog();

namespace fs = std::filesystem;

TopNodeType isTSNodeTop(std::string node_type);
SecondNodeType isTSNodeSecond(std::string node_type);
bool isTSNodeNet(std::string node_type);
void CollectSigTreeNodeInfoTS(SigTreeNode* node, TSNode& TSnode, std::string filepath, std::string code);

SigFlowTree::SigFlowTree(std::string projectPath) {
    fs::path fullPath = fs::path(projectPath) / "sigflow.project";

    std::ifstream file(fullPath, std::ios::in | std::ios::binary);
    if (!file.is_open()) {
        // 抛出异常
        return;
    }
    std::string utf8Content((std::istreambuf_iterator<char>(file)),
        std::istreambuf_iterator<char>());

    Json::Value root;
    Json::CharReaderBuilder builder;
    std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    std::string errs;

    if (!reader->parse(utf8Content.c_str(), utf8Content.c_str() + utf8Content.size(), &root, &errs)) {
        return;
    }

    this->root = arena.make<ProjectNode>(projectPath);

    if (root["paths"].isMember("source_files")) {
        for (const auto& file : root["paths"]["source_files"]) {
            this->root->children.push_back(arena.make<FileNode>(file.asString()));
        }
    }
    if (root["paths"].isMember("library_files")) {
        for (const auto& file : root["paths"]["library_files"]) {
            this->root->children.push_back(arena.make<FileNode>(file.asString()));
        }
    }

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

        )";

    second = ts_query_new(tree_sitter_verilog(), sec_temp, strlen(sec_temp), &net_error_offset, &net_error_type);


    const char* net_temp = R"(
                    (net_declaration
                        (net_type) @net.type
                        (list_of_net_decl_assignments
                            (net_decl_assignment
                                (simple_identifier)@net.name)))

        )";

    net = ts_query_new(tree_sitter_verilog(), net_temp, strlen(net_temp), &net_error_offset, &net_error_type);


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
                NetNode* nn = arena.make<NetNode>();
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
                        nn->parent = newParent;
                        newParent->children.push_back(nn);
                    }

                }
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
                    p.direction = PortDirection::Ref;

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
            }
            else if (second_type == SecondNodeType::GateInstance) {
                TSQueryCursor* cursor = ts_query_cursor_new();
                ts_query_cursor_exec(cursor, second, currentNode);

                TSQueryMatch match;
                int gate_input_count = 1;
                int gate_output_count = 1;
                while (ts_query_cursor_next_match(cursor, &match)) {
                    Port p;
                    p.direction = PortDirection::Ref;

                    for (uint16_t i = 0; i < match.capture_count; i++) {
                        TSQueryCapture capture = match.captures[i];

                        uint32_t name_len;
                        const char* tag_name = ts_query_capture_name_for_id(second, capture.index, &name_len);

                        if (strcmp(tag_name, "gate.type") == 0) {
                            TSNode nameNode = capture.node;
                            sn->gatetype = code.substr(ts_node_start_byte(capture.node),
                                ts_node_end_byte(capture.node) - ts_node_start_byte(capture.node));
                        }
                        else if (strcmp(tag_name, "gate.name") == 0) {
                            TSNode nameNode = capture.node;
                            sn->identifier = code.substr(ts_node_start_byte(capture.node),
                                ts_node_end_byte(capture.node) - ts_node_start_byte(capture.node));
                        }
                        else if (strcmp(tag_name, "gate.output") == 0) {
                            TSNode nameNode = capture.node;
                            p.identifier = std::format("out{}", gate_output_count++);
                            p.direction = PortDirection::Out;
                            p.conn = code.substr(ts_node_start_byte(capture.node),
                                ts_node_end_byte(capture.node) - ts_node_start_byte(capture.node));
                            sn->ports.push_back(p);
                        }
                        else if (strcmp(tag_name, "gate.input") == 0) {
                            TSNode nameNode = capture.node;
                            p.identifier = std::format("in{}", gate_input_count++);
                            p.direction = PortDirection::In;
                            p.conn = code.substr(ts_node_start_byte(capture.node),
                                ts_node_end_byte(capture.node) - ts_node_start_byte(capture.node));
                            sn->ports.push_back(p);
                        }
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
    else if(node_type == "udp_instantiation") return SecondNodeType::UDPInstance;
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



void SigFlowTree::BuildSigSubTree(TSTreeCursor* cursor, SigTreeNode* parentSigNode) {

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
                else if (child2->type == SigTreeNodeType::Net) {
                    NetNode* nn = static_cast<NetNode*>(child2);
                    nn->PrintNetNode();
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
    case SigTreeNodeType::Net:
        info += "Net";
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
        break;
    case SecondNodeType::UDPInstance:
        info += "SecondType: UDPInstance\n";
        break;
    case SecondNodeType::ContinuousAssign:
        info += "SecondType: ContinuousAssign\n";
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

void NetNode::PrintNetNode() {
    SigTreeNode::PrintSigTreeNode();
    std::string info;

    info = "Identifier: " + identifier + "\n";

    OutputDebugStringA(info.c_str());

}
