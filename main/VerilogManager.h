#pragma once
#include <vector>
#include <list>
#include <unordered_map>
#include <wx/stc/stc.h>
#include <tree_sitter/api.h>
#include <wx/wx.h>
#include "TreeSitterLinter.h"

class SigTreeNode;
class SigFlowTree;
class SigTextEditor;
class FileNode;
class Structuring;

struct Block {
    int startHandle;
    int endHandle;
    SigTreeNode* self;
    Structuring* structure;
    Block(int start, int end, SigTreeNode* node)
        : startHandle(start), endHandle(end), self(node), structure(nullptr) {};
    bool isStable() { return self != nullptr; }
};

class VerilogManager {
public:
    FileNode* fn;
    SigTextEditor* m_stc;
    std::list<Block> blocks;
    std::list<Block> break_blocks;
    std::list<Structuring> structures;
    SigFlowTree* m_tree;

    //std::vector<Block*> highlighted_blocks;
    Block* editing_top_block;
    Block* recovering_block;

    wxTimer* m_timer;

    TSTree* m_ts_tree;
    TSParser* m_ts_parser;



    VerilogManager(SigTextEditor* stc, SigFlowTree* tree, TSParser* parser);
    ~VerilogManager();
    void ClearFileNode();
    bool SetFileNode(FileNode* n, std::unordered_map<SigTreeNode*, std::tuple<int, int>> map);

    void CollectBlocks(std::unordered_map<SigTreeNode*, std::tuple<int, int>>& map);
    void AppendBlocks(std::unordered_map<SigTreeNode*, std::tuple<int, int>>& map);

    void AddBlock(int startline, int endline, SigTreeNode* node);
    Block* AddBreakBlock(int startline, int endline);

    void RecoverBlock(Block* b);
    void RecoverBreakBlock(Block* b, SigTreeNode* n);
    Block* FindBlock(int start, int end);

    
    std::list<Block>& GetBreakBlocks() { return break_blocks; };
    int GetLine(int handle);
    int GetSTCLine(int handle);
    wxString GetText(const Block& b);
    wxString GetBlockText(const Block& b);
    void Print();

    void SigFlowNodeAdded(SigTreeNode* node);
    void SigFlowNodeDeleted(SigTreeNode* node);

    std::vector<Stability> GetLineStatus();
    void UpdateFolding();
    void DrawBlockInfo();

    void EditBlock(wxStyledTextEvent& event);
    void OnTimer(wxTimerEvent&);
};
