#include "TreeSitterLinter.h"
#include <wx/file.h>
#include <format>
#include <unordered_set>

extern "C" TSLanguage* tree_sitter_verilog();


void DumpTree(TSNode node, const wxString& src, int indent = 0);
std::string LoadCodeFromPath(const wxString& path);
bool contains_missing(TSNode node);
void PrintTreeSitterResult(const std::vector<BlockInfo>& res);
std::string GetBlockTypeStr(const BlockType type);
std::vector<int> TraverseNode(TSNode node, const char* source_buffer, std::vector<BlockInfo>& blocks, int& id);


TreeSitterLinter::TreeSitterLinter() {
    parser = ts_parser_new();
    ts_parser_set_language(parser, tree_sitter_verilog());
}

TreeSitterLinter::~TreeSitterLinter() {
    if (parser) ts_parser_delete(parser);
    if (tree) ts_tree_delete(tree);
}


std::vector<BlockInfo> TreeSitterLinter::Lint(wxString code) {
    //GetAllNodeTypeTS();
    std::vector<BlockInfo> res;

    TSTree* new_tree = ts_parser_parse_string(parser, nullptr, code.c_str(), code.length());
    //if (tree) ts_tree_delete(tree);
    //tree = new_tree;
    TSNode root_node = ts_tree_root_node(new_tree);
    int id_count = 1;
    //DumpTree(root_node, code);
    TraverseNode(root_node, code, res, id_count);

    std::sort(res.begin(), res.end(), [](const BlockInfo& a, const BlockInfo& b) {
        return a.id < b.id; // 按 ID 从小到大排序
        });
    ts_tree_delete(new_tree);
    return res;
}



std::vector<BlockInfo> TreeSitterLinter::LintFromPath(wxString filePath) {
    wxString code = LoadCodeFromPath(filePath);
    std::vector<BlockInfo> res = Lint(code);
    return res;
}

void GetAllNodeTypeTS() {
    const TSLanguage* lang = tree_sitter_verilog();

    uint32_t symbol_count = ts_language_symbol_count(lang); // 使用 API 获取数量
    for (uint32_t i = 0; i < symbol_count; ++i) {
        const char* name = ts_language_symbol_name(lang, i); // 获取 symbol 名称
        //OutputDebugStringA(wxString::Format("%s\n", name));
    }
}


std::string GetNodeName(TSNode node, const char* source_buffer) {
    // 遍历节点的所有子节点
    uint32_t child_count = ts_node_named_child_count(node);
    for (uint32_t i = 0; i < child_count; ++i) {
        TSNode child = ts_node_named_child(node, i);
        const char* type = ts_node_type(child);

        if (strcmp(type, "simple_identifier") == 0) {
            uint32_t start = ts_node_start_byte(child);
            uint32_t end = ts_node_end_byte(child);
            return std::string(source_buffer + start, end - start);
        }
        else {
            // 递归查找
            std::string name = GetNodeName(child, source_buffer);
            if (!name.empty()) return name;
        }
    }

    return ""; // 没找到
}



Stability GetBlockStability(TSNode node) {
    if (ts_node_has_error(node)) return Stability::Corrupted;
    else if (ts_node_is_missing(node)) return Stability::Incomplete;
    else return Stability::Stable;
}











std::string LoadCodeFromPath(const wxString& path)
{
    std::string code;
    wxFile file(path, wxFile::read);
    if (!file.IsOpened()) return code;

    // 2. 获取长度并读取
    wxFileOffset len = file.Length();
    if (len == 0) return code;

    code.resize(len);
    if (file.Read(&code[0], len) != len) {
        return "";
    }
    file.Close();
    return code;
}

/*
void DumpTree(TSNode node, const wxString& src, int indent) {
    wxString line;

    line << "|-";
    for (int i = 0; i < indent; ++i)
        line << "-";

    line  << "{" << indent << "L}" << ts_node_type(node);

    if (ts_node_is_named(node))
        line << " [named]";

    if (ts_node_is_missing(node)) line << " [missing]";
    if (ts_node_has_error(node)) line << " [has error]";
    if(ts_node_is_error(node)) line << " [error]";
    

    line << "  (" << ts_node_start_byte(node)
        << "," << ts_node_end_byte(node) << ")";

    line << "  text=\""
        << src.substr(ts_node_start_byte(node),
            ts_node_end_byte(node) - ts_node_start_byte(node))
        << "\n";

    wxLogDebug("%s", line);
    //OutputDebugStringA(line);

    uint32_t n = ts_node_child_count(node);
    for (uint32_t i = 0; i < n; ++i)
        DumpTree(ts_node_child(node, i), src, indent + 1);
}
*/


bool contains_missing(TSNode node) {
    if (ts_node_is_missing(node)) return true;
    uint32_t n = ts_node_child_count(node);
    for (uint32_t i = 0; i < n; i++) {
        if (contains_missing(ts_node_child(node, i)))
            return true;
    }
    return false;
}

void PrintTreeSitterResult(const std::vector<BlockInfo>& res) {
    wxString out;
    for (size_t i = 0; i < res.size(); ++i) {
        const auto& b = res[i];

        std::string kindStr = GetBlockTypeStr(b.type);

        std::string stabStr;
        switch (b.stability) {
        case Stability::Stable:     stabStr = "Stable"; break;
        case Stability::Incomplete: stabStr = "Incomplete"; break;
        case Stability::Corrupted:  stabStr = "Corrupted"; break;
        }

        wxString line = "";
        //if (!b.name.empty())
        line += std::format("[{}] {}",
            b.id, kindStr);
        if (!b.name.empty()) line += std::format("\"{}\"", b.name);


        line += std::format(" @ {}:{}-{}:{}",
            b.start_line, b.start_col,
            b.end_line, b.end_col);

        line += std::format("\tchildrens: ");
        for (int id : b.child_ids) {
            line += std::format("[{}] ", id);
        }

        line += std::format("({})\n", stabStr);
        //else 


        out += line;
    }
    //OutputDebugStringA(out);
    //wxLogDebug(out);
}

BlockType GetBlockType(const std::string& item_type) {

    static const std::unordered_map<std::string, BlockType> typeMap = {
            {"module_declaration",      BlockType::Module},
            {"udp_declaration",      BlockType::UDP},

            {"always_construct",      BlockType::Always},
            {"attribute_instance",   BlockType::Attribute},

            {"case_generate_construct",       BlockType::Case},
            {"case_statement",       BlockType::Case},

            {"loop_generate_construct",       BlockType::Loop},
            {"loop_statement",       BlockType::Loop},

            {"combinational_body",       BlockType::TruthTable},
            {"sequential_body",       BlockType::TruthTable},

            {"continuous_assign",        BlockType::Assign},
            {"blocking_assignment",        BlockType::Assign},
            {"nonblocking_assignment",        BlockType::Assign},
            {"procedural_continuous_assignment",        BlockType::Assign},
            {"defparam_assignment",        BlockType::Assign},

            {"gate_instantiation",       BlockType::Gate_Ins},
            {"module_instantiation",       BlockType::Module_Ins},
            {"udp_instantiation",       BlockType::UDP_Ins},

            {"genvar_declaration",    BlockType::Genvar},
            {"parameter_override",    BlockType::DefParam},

            {"initial_construct",         BlockType::Initial},

            {"if_generate_construct",         BlockType::If},
            {"conditional_statement",         BlockType::If},

            {"data_declaration",         BlockType::Data},
            {"net_declaration",         BlockType::Net},

            {"generate_block",         BlockType::Generate},
            {"seq_block",         BlockType::Sequence},

            {"task_declaration",         BlockType::Task},
            {"function_declaration",         BlockType::Function},

            {"parameter_declaration", BlockType::Param},
            {"local_parameter_declaration", BlockType::Param},
            {"specparam_declaration", BlockType::Param},
    };
    auto it = typeMap.find(item_type);
    return it == typeMap.end() ? BlockType::Unknown : it->second;

}


std::string GetBlockTypeStr(const BlockType type) {

    static const std::unordered_map<BlockType, std::string> BlockDescMap = {
        {BlockType::Module,       "Module Definition"},
        {BlockType::Generate,     "Generate Construct"},
        {BlockType::Sequence,     "Sequential Block"},
        {BlockType::UDP,          "User Defined Primitive"},
        {BlockType::Always,       "Always Construct"},
        {BlockType::Initial,      "Initial Construct"},
        {BlockType::Task,         "Task Declaration"},
        {BlockType::Function,     "Function Declaration"},
        {BlockType::Assign,       "Continuous Assignment"},
        {BlockType::Gate_Ins,     "Gate Instantiation"},
        {BlockType::Module_Ins,   "Module Instantiation"},
        {BlockType::UDP_Ins,      "UDP Instantiation"},
        {BlockType::Param,        "Parameter Declaration"},
        {BlockType::DefParam,     "Parameter Override"},
        {BlockType::Genvar,       "Genvar Declaration"},
        {BlockType::Data,         "Variable Declaration"},
        {BlockType::Net,          "Net Declaration"},
        {BlockType::If,           "Conditional Statement"},
        {BlockType::Case,         "Case Statement"},
        {BlockType::Loop,         "Loop Statement"},
        {BlockType::Attribute,    "Attribute Instance"},
        {BlockType::TruthTable,   "Truth Table Body"},
        {BlockType::Unknown,      "Unknown Type"}
    };
    auto it = BlockDescMap.find(type);
    return it == BlockDescMap.end() ? "Unknown Type" : it->second;

}

bool CollectBlockInfo(TSNode node, BlockInfo* block) {
    // 不处理id, child_ids, name, type

    block->stability = GetBlockStability(node);

    block->start_byte = ts_node_start_byte(node);
    block->end_byte = ts_node_end_byte(node);
    TSPoint start = ts_node_start_point(node); // {row, column}
    TSPoint end = ts_node_end_point(node);

    block->start_line = start.row + 1;
    block->start_col = start.column + 1;
    block->end_line = end.row + 1;
    block->end_col = end.column + 1;

    return true;
}
void printLineStatus(std::vector<Stability> status) {
    int id = 0;
    for (auto& s : status) {
        wxString sta;
        switch (s) {
        case Stability::Corrupted:
            sta = "Corrupted";
            break;
        case Stability::Incomplete:
            sta = "Incomplete";
            break;
        case Stability::Stable:
            sta = "Stable";
            break;
        }
        if (id == 0) {
            id++;
            continue;
        }
        //OutputDebugStringA(wxString::Format("Line [%d] : %s \n", id++,sta));
    }
}





std::vector<int> TraverseNode(TSNode node, const char* source_buffer, std::vector<BlockInfo>& blocks, int& id) {
    std::vector<int> ids;
    if (ts_node_is_null(node)) return ids;
    BlockInfo block;

    std::string item_type = ts_node_type(node);
    BlockType type = GetBlockType(item_type);
    if (type != BlockType::Unknown) {
        block.id = id++;
        block.name = GetNodeName(node, source_buffer);
        block.type = type;
        block.stability = GetBlockStability(node);
        CollectBlockInfo(node, &block);

        uint32_t child_count = ts_node_named_child_count(node);
        for (uint32_t i = 0; i < child_count; ++i) {
            TSNode child = ts_node_named_child(node, i);
            std::vector<int> child_ids = TraverseNode(child, source_buffer, blocks, id);
            block.child_ids.insert(block.child_ids.end(), child_ids.begin(), child_ids.end());
        }
        blocks.push_back(block);
        ids.push_back(block.id);
    }
    else {
        uint32_t child_count = ts_node_named_child_count(node);
        for (uint32_t i = 0; i < child_count; ++i) {
            TSNode child = ts_node_named_child(node, i);
            std::vector<int> child_ids = TraverseNode(child, source_buffer, blocks, id);
            ids.insert(ids.end(), child_ids.begin(), child_ids.end());
        }
    }
    return ids;
}








