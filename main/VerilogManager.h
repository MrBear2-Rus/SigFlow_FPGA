#pragma once
#include <vector>
#include <list>
#include <memory>
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
        : startHandle(start), endHandle(end), self(node), structure(nullptr) {
    };
    bool isStable() const { return self != nullptr; }
};

class VerilogManager {
public:
    FileNode* fn;
    SigTextEditor* m_stc;
    std::vector<Block> blocks;
    std::vector<Block> break_blocks;
    // 使用 std::list 保证 Structuring 节点地址稳定，避免 vector 重分配让 Block::structure 指针失效。
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
    void SetFileNode(FileNode* n, std::unordered_map<SigTreeNode*, std::tuple<int, int>> map);

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

    // 异常块管控：在 EditBlock 触发时主动清理死亡块、修复半死亡块、
    // 重建/迁移单行块的 endHandle 以保证 Scintilla marker 与 Block 语义一致。
    void HousekeepBlocks(int eventType, int pos, int len, const wxString& text);
    void HousekeepVector(std::vector<Block>& vec,
                         int eventType, int pos, int len, int newlineCount);
    // 销毁单个 Block：同步从 SFTree 删除节点、销毁双端 marker、释放 structure。
    void DestroyBlock(Block& b);
};
