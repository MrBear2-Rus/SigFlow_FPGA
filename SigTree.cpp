#include "SigTree.h"

#include <json/json.h>
#include <iostream>
#include <fstream>
#include <filesystem>
#include <memory>
#include <windows.h>
#include <regex>

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

            //TSNode idNode = ts_node_child_by_field_name(currentNode, "name", 4);
            //if (ts_node_is_null(idNode)) {
            //    tn->identifier = "<unnamed>";
            //}
            //else {
            //    tn->identifier = code.substr(ts_node_start_byte(idNode),
            //        ts_node_end_byte(idNode) - ts_node_start_byte(idNode));
            //}
            for (int i = 0; i < ts_node_child_count(currentNode); i++) {
                TSNode module_header = ts_node_child(currentNode, i);
                if (ts_node_type(module_header) == "module_header") {
                    for (int j = 0; j < ts_node_child_count(module_header); j++) {
                        TSNode simple_identifier = ts_node_child(module_header, j);
                        if (ts_node_type(simple_identifier) == "simple_identifier") {
                            tn->identifier = code.substr(ts_node_start_byte(simple_identifier),
                                ts_node_end_byte(simple_identifier) - ts_node_start_byte(simple_identifier));
                            break;
                        }
                    }
                }
            }
            




            TSNode portsNode = ts_node_child_by_field_name(currentNode, "ports", 5);
            if (!ts_node_is_null(portsNode)) {
                // 遍历端口列表括号里的每一个子节点
                uint32_t portCount = ts_node_child_count(portsNode);
                for (uint32_t i = 0; i < portCount; i++) {
                    TSNode portChild = ts_node_child(portsNode, i);
                    if (std::string(ts_node_type(portChild)) == "ansi_port_declaration") {
                        // 调用你封装好的解析函数
                        Port p;
                        // 使用 Field 拿 direction
                        TSNode dirNode = ts_node_child_by_field_name(portChild, "direction", 9);
                        if (!ts_node_is_null(dirNode)) {
                            std::string d = code.substr(ts_node_start_byte(dirNode),
                                ts_node_end_byte(dirNode) - ts_node_start_byte(dirNode));

                            if (d == "input") p.direction = PortDirection::In;
                            else if (d == "output") p.direction = PortDirection::Out;
                        }
                        // 使用 Field 拿 name
                        TSNode pidNode = ts_node_child_by_field_name(portChild, "name", 4);
                        if (ts_node_is_null(pidNode)) {
                            p.identifier = "<unnamed>";
                        }
                        else {
                            p.identifier = code.substr(ts_node_start_byte(pidNode),
                                ts_node_end_byte(pidNode) - ts_node_start_byte(pidNode));
                        }
                        tn->ports.push_back(p);
                    }
                }
            }


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
            NetNode* nn = arena.make<NetNode>();
            //TSNode idNode = ts_node_child_by_field_name(currentNode, "name", 4);
            //if (ts_node_is_null(idNode)) {
            //    nn->identifier = "<unnamed>";
            //}
            //else {
            //    nn->identifier = code.substr(ts_node_start_byte(idNode),
            //        ts_node_end_byte(idNode) - ts_node_start_byte(idNode));
            //}
            for (int i = 0; i < ts_node_child_count(currentNode); i++) {
                TSNode simple_identifier = ts_node_child(currentNode, i);
                if (ts_node_type(simple_identifier) == "simple_identifier") {
                    nn->identifier = code.substr(ts_node_start_byte(simple_identifier),
                        ts_node_end_byte(simple_identifier) - ts_node_start_byte(simple_identifier));
                    break;
                }
            }

            nn->parent = newParent;
            CollectSigTreeNodeInfoTS(nn, currentNode, filePath, code);
            newParent->children.push_back(nn);

        }
        // 找Second
        else if (second_type != SecondNodeType::Null) {
            SecondNode* sn = arena.make<SecondNode>();
            sn->secondType = second_type;

            //TSNode idNode = ts_node_child_by_field_name(currentNode, "name", 4);
            //if (ts_node_is_null(idNode)) {
            //    sn->identifier = "<unnamed>";
            //}
            //else {
            //    sn->identifier = code.substr(ts_node_start_byte(idNode),
            //        ts_node_end_byte(idNode) - ts_node_start_byte(idNode));
            //}

            for (int i = 0; i < ts_node_child_count(currentNode); i++) {
                TSNode s = ts_node_child(currentNode, i);
                if (ts_node_type(s) == "simple_identifier") {
                    sn->identifier = code.substr(ts_node_start_byte(s),
                        ts_node_end_byte(s) - ts_node_start_byte(s));
                    break;   
                }
                else if (ts_node_type(s) == "list_of_net_decl_assignments") {
                    for (int j = 0; j < ts_node_child_count(s); j++) {
                        TSNode net_dec_ass = ts_node_child(s, j);
                        for (int z = 0; z < ts_node_child_count(net_dec_ass); z++) {
                            TSNode simple_identifier = ts_node_child(net_dec_ass, z);
                            if (ts_node_type(simple_identifier) == "simple_identifier") {
                                sn->identifier = code.substr(ts_node_start_byte(simple_identifier),
                                    ts_node_end_byte(simple_identifier) - ts_node_start_byte(simple_identifier));
                                break;
                            }
                        }
                    }
                    
                }
            }

            

            TSNode portsNode = ts_node_child_by_field_name(currentNode, "ports", 5);
            if (!ts_node_is_null(portsNode)) {
                // 遍历端口列表括号里的每一个子节点
                uint32_t portCount = ts_node_child_count(portsNode);
                for (uint32_t i = 0; i < portCount; i++) {
                    TSNode portChild = ts_node_child(portsNode, i);
                    if (std::string(ts_node_type(portChild)) == "ansi_port_declaration") {
                        // 调用你封装好的解析函数
                        Port p;
                        // 使用 Field 拿 direction
                        TSNode dirNode = ts_node_child_by_field_name(portChild, "direction", 9);
                        if (!ts_node_is_null(dirNode)) {
                            std::string d = code.substr(ts_node_start_byte(dirNode),
                                ts_node_end_byte(dirNode) - ts_node_start_byte(dirNode));

                            if (d == "input") p.direction = PortDirection::In;
                            else if (d == "output") p.direction = PortDirection::Out;
                        }
                        // 使用 Field 拿 name
                        TSNode pidNode = ts_node_child_by_field_name(portChild, "name", 4);
                        if (ts_node_is_null(pidNode)) {
                            p.identifier = "<unnamed>";
                        }
                        else {
                            p.identifier = code.substr(ts_node_start_byte(pidNode),
                                ts_node_end_byte(pidNode) - ts_node_start_byte(pidNode));
                        }
                        sn->ports.push_back(p);
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
        info += "port: " + p.identifier + " " + std::string(p.direction == PortDirection::In ? "In" : "Out") + "\n";
    }

    OutputDebugStringA(info.c_str());

}

void NetNode::PrintNetNode() {
    SigTreeNode::PrintSigTreeNode();
    std::string info;

    info = "Identifier: " + identifier + "\n";

    OutputDebugStringA(info.c_str());

}
