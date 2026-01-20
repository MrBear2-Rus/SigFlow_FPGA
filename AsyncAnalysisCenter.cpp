#include "AsyncAnalysisCenter.h"

#include <tree_sitter/api.h>
#include <regex>
#include <wx/file.h>
#include <wx/process.h>
#include <wx/txtstrm.h>
#include <windows.h>
#include <iostream>
#include <string>
#include <sstream>
#include <wx/filename.h>
#include <unordered_set>
#include <wx/wfstream.h>
#include <wx/sstream.h>
#include <wx/textfile.h>
#include <stack>



#include <slang/syntax/SyntaxTree.h>
#include <slang/ast/Compilation.h>
#include <slang/diagnostics/DiagnosticEngine.h>
#include <slang/text/SourceManager.h>

// 声明外部 Verilog 语言支持
extern "C" TSLanguage* tree_sitter_verilog();

// 定义事件 ID
wxDEFINE_EVENT(EVT_ANALYSIS_COMPLETE, wxThreadEvent);


bool contains_missing(TSNode node);
void PrintTreeSitterResult(const TreeSitterResult& res);
std::string GetBlockTypeStr(const BlockType type);
std::vector<Stability> GetLineStatus(
    const std::vector<BlockInfo>& blocks,
    int code_line_count
);
std::vector<int> GetStableLines(const std::vector<BlockInfo>& blocks);
std::vector<int> GetLineDepth(const std::vector<BlockInfo>& blocks, int total_lines);
std::vector<bool> isLineHeader(const std::vector<BlockInfo>& blocks, int total_lines);

void DumpTree(TSNode node, const wxString& src, int indent = 0) {
    wxString line;

    for (int i = 0; i < indent; ++i)
        line << "  ";

    line << ts_node_type(node);

    if (ts_node_is_named(node))
        line << " [named]";

    line << "  (" << ts_node_start_byte(node)
        << "," << ts_node_end_byte(node) << ")";

    line << "  text=\""
        << src.substr(ts_node_start_byte(node),
            ts_node_end_byte(node) - ts_node_start_byte(node))
        << "\n";

    //wxLogDebug("%s", line);
    OutputDebugStringA(line);

    uint32_t n = ts_node_child_count(node);
    for (uint32_t i = 0; i < n; ++i)
        DumpTree(ts_node_child(node, i), src, indent + 1);
}



AsyncAnalysisCenter::AsyncAnalysisCenter(wxEvtHandler* parentHandler)
    : m_parentHandler(parentHandler), m_hasNewTask(false)
{
    parser = ts_parser_new();
    ts_parser_set_language(parser, tree_sitter_verilog());

    //atomsAnalysis = new AtomAnalysis();
}

AsyncAnalysisCenter::~AsyncAnalysisCenter() {
    // 确保线程在对象销毁前安全退出
    if (GetThread() && GetThread()->IsRunning()) {
        GetThread()->Wait();
    }
    if (parser) ts_parser_delete(parser);
}

void AsyncAnalysisCenter::PushTask(const wxString& projectPath, const wxString& filePath) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_projectPath = projectPath;
    m_pendingPath = filePath; // 记录待分析的文件路径 哈哈
    m_hasNewTask = true;

    wxThread* thread = GetThread();
    if (!thread || !thread->IsRunning()) {
        if (CreateThread(wxTHREAD_DETACHED) == wxTHREAD_NO_ERROR) {
            GetThread()->Run();
        }
    }
}

wxThread::ExitCode AsyncAnalysisCenter::Entry() {
    while (!GetThread()->TestDestroy()) {
        wxString filePath;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            // 改进：使用条件变量代替 Sleep，提高响应速度
            if (!m_hasNewTask) {
                wxMilliSleep(50); // 或者使用 m_condition.wait_for
                continue;
            }
            filePath = m_pendingPath;
            m_hasNewTask = false;
        }

        int line_count;
        wxTextFile file;
        if (file.Open(filePath)) {
            size_t count = file.GetLineCount();
            file.Close();
            line_count = static_cast<int>(count);
        }

        //LintRes finalRes = Lint(pathToProcess);
        LintResult res;
        TreeSitterResult TSRes = TreeSitterFromPath(filePath);
        PrintTreeSitterResult(TSRes);
        res.block_infos = TSRes.block_infos;
        res.is_lines_header = isLineHeader(TSRes.block_infos, line_count);
        res.line_depth = GetLineDepth(TSRes.block_infos, line_count);
        res.line_status = GetLineStatus(TSRes.block_infos, line_count);
        res.stable_lines = GetStableLines(TSRes.block_infos);

        // 包装结果并推回 UI 线程
        wxThreadEvent* event = new wxThreadEvent(EVT_ANALYSIS_COMPLETE);

        atomsAnalysis.AnalyzeProject(m_projectPath);


        event->SetPayload(res); // 现在 Payload 是 LintRes
        m_parentHandler->QueueEvent(event);
    }
    return (wxThread::ExitCode)0;
}

// 辅助函数：递归提取节点下所有的标识符 (Atoms)
void CollectAtoms(TSNode node, const std::string& code, std::vector<std::string>& atoms) {
    if (ts_node_is_null(node)) return;

    // 检查是否为标识符节点
    const char* type = ts_node_type(node);
    if (std::string(type) == "simple_identifier") {
        uint32_t start = ts_node_start_byte(node);
        uint32_t end = ts_node_end_byte(node);
        std::string name = code.substr(start, end - start);

        // 简单去重：如果已经在 atoms 里了就不再添加
        if (std::find(atoms.begin(), atoms.end(), name) == atoms.end()) {
            atoms.push_back(name);
        }
    }

    // 递归处理子节点
    uint32_t child_count = ts_node_child_count(node);
    for (uint32_t i = 0; i < child_count; ++i) {
        CollectAtoms(ts_node_child(node, i), code, atoms);
    }
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




bool contains_missing(TSNode node) {
    if (ts_node_is_missing(node)) return true;
    uint32_t n = ts_node_child_count(node);
    for (uint32_t i = 0; i < n; i++) {
        if (contains_missing(ts_node_child(node, i)))
            return true;
    }
    return false;
}

void PrintTreeSitterResult(const TreeSitterResult& res) {
    wxString out;
    for (size_t i = 0; i < res.block_infos.size(); ++i) {
        const auto& b = res.block_infos[i];

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

        line += std::format("({})\n",stabStr);
        //else 


        out += line;
    }
    OutputDebugStringA(out);
    //wxLogDebug(out);
}

void GetAllNodeTypeTS() {
    const TSLanguage* lang = tree_sitter_verilog();

    uint32_t symbol_count = ts_language_symbol_count(lang); // 使用 API 获取数量
    for (uint32_t i = 0; i < symbol_count; ++i) {
        const char* name = ts_language_symbol_name(lang, i); // 获取 symbol 名称
        OutputDebugStringA(wxString::Format("%s\n", name));
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





BlockType GetBlockType(const std::string & item_type) {

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
        case Stability::Corrupted :
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
        OutputDebugStringA(wxString::Format("Line [%d] : %s \n", id++,sta));
    }
}

std::vector<Stability> GetLineStatus(
    const std::vector<BlockInfo>& blocks,
    int code_line_count
) {
    // 1. 初始化所有行为 Stable (长度设为 line_count + 1 以对齐行号)
    std::vector<Stability> status(code_line_count+1, Stability::Stable);

    for (const auto& block : blocks) {
        if (block.child_ids.empty()) {
            for (int i = block.start_line; i <= block.end_line; ++i) {
                if (i >= 0 && i < code_line_count) {
                    if (block.stability == Stability::Stable) continue;
                        status[i] = block.stability;
                }
            }
        }
    }

    printLineStatus(status);
    return status;
}

std::vector<int> GetStableLines(const std::vector<BlockInfo>& blocks) {
    std::vector<int> out;
    std::unordered_set<int> reject;
    for (auto& b : blocks) {
        for (auto& c : b.child_ids) reject.insert(c);
    }

    for (auto& b : blocks) {
        if (reject.find(b.id) == reject.end() && b.stability == Stability::Stable) {
            for (int i = b.start_line; i <= b.end_line; i++) {
                out.push_back(i);
                OutputDebugStringA(wxString::Format("[%d] ", i));
            }
            
        }
    }


    return out;
}

std::vector<int> GetLineDepth(const std::vector<BlockInfo>& blocks, int total_lines) {
    std::vector<int> depths(total_lines + 1, -1);
    //for (int i = 1; i <= total_lines; i++) {

    //}
    
    for (const auto& b : blocks) {
        if (b.start_line == b.end_line) /*depths[b.start_line]++*/;
        else {
            depths[b.start_line]++;
            depths[b.end_line]++;
            for (int i = b.start_line + 1; i < b.end_line; i++) {
                depths[i] = depths[i] + 2;
            }
        }
    }
    for (int i = 1; i <= total_lines; i++) {
        if (depths[i] == -1) depths[i] = 0;
    }
    
    


    for (int i = 1; i <= total_lines; i++) {
        OutputDebugStringA(wxString::Format("Line [%d]: Depth %d\n",i, depths[i]));
    }
    return depths;
}

std::vector<bool> isLineHeader(const std::vector<BlockInfo>& blocks, int total_lines) {
    // 1. 初始化差分数组
    std::vector<bool> mark(total_lines+1, false);

    // 2. 每一个 Block 都是一个 [start, end] 的全覆盖区间
    for (const auto& b : blocks) {
        if(!(b.start_line==b.end_line)) mark[b.start_line] = true;
    }

    
    for (int i = 1; i <= total_lines; i++) {
        OutputDebugStringA(wxString::Format("Line [%d]: Header? %d\n", i, mark[i]? 1:0));
    }
    return mark;
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




TreeSitterResult AsyncAnalysisCenter::TreeSitter(wxString code) {
    //GetAllNodeTypeTS();
    TreeSitterResult res;

    TSTree* new_tree = ts_parser_parse_string(parser, nullptr, code.c_str(), code.length());
    //if (tree) ts_tree_delete(tree);
    //tree = new_tree;
    TSNode root_node = ts_tree_root_node(new_tree);
    int id_count = 1;
    DumpTree(root_node, code);
    TraverseNode(root_node, code, res.block_infos, id_count);

    std::sort(res.block_infos.begin(), res.block_infos.end(), [](const BlockInfo& a, const BlockInfo& b) {
        return a.id < b.id; // 按 ID 从小到大排序
        });
    ts_tree_delete(new_tree);
    return res;
}


TreeSitterResult AsyncAnalysisCenter::TreeSitterFromPath(wxString filePath) {
    wxString code = LoadCodeFromPath(filePath);
    TreeSitterResult res = TreeSitter(code);
    return res;
}

