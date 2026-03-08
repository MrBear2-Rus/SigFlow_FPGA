#pragma once
#include <wx/wx.h>
#include <string>
#include <mutex>
#include <set>
#include <map>
#include <vector>

#include <tree_sitter/api.h>

enum class BlockType { Module, UDP, Module_Ins, Gate_Ins, UDP_Ins, TruthTable, Assign, Always, Initial, Generate, Sequence, Attribute, If, Case, Loop, Genvar, DefParam, Net, Data, Task, Function, Param, Unknown };
enum class Stability { Stable, Incomplete, Corrupted };


struct BlockInfo {
    int id;

    std::string name;
    BlockType type;
    Stability stability;

    uint32_t start_byte;
    uint32_t end_byte;

    uint32_t start_line;
    uint32_t start_col;
    uint32_t end_line;
    uint32_t end_col;

    std::vector<int> child_ids;
};


class TreeSitterLinter {
public:
    TreeSitterLinter();
    ~TreeSitterLinter();

    bool TSTest(wxString code);
    std::tuple<TSNode, bool> GetStructNode(wxString code);
    std::vector<BlockInfo> Lint(wxString code);
    std::vector<BlockInfo> LintFromPath(wxString filePath);

private:

    TSParser* parser = nullptr;
    TSTree* tree = nullptr;
};




