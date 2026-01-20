#pragma once
#include <wx/wx.h>
#include <wx/thread.h>
#include <string>
#include <mutex>
#include <set>
#include <map>

#include <tree_sitter/api.h>

#include "AtomsAnalysis.h"

enum class BlockType { Module, UDP,  Module_Ins, Gate_Ins, UDP_Ins, TruthTable, Assign, Always, Initial, Generate, Sequence, Attribute, If, Case, Loop,  Genvar, DefParam, Net, Data, Task, Function, Param, Unknown};
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

struct TreeSitterResult {
    std::vector<BlockInfo> block_infos;
};

struct SlangResult {

};

struct VerilatorResult {

};

struct LintResult {
    std::vector<BlockInfo> block_infos;
    std::vector<bool> is_lines_header;
    std::vector<int> line_depth;
    std::vector<Stability> line_status;
    std::vector<int> stable_lines;
};

// --- 自定义事件定义 ---
wxDECLARE_EVENT(EVT_ANALYSIS_COMPLETE, wxThreadEvent);

class AsyncAnalysisCenter : public wxThreadHelper {
public:
    AsyncAnalysisCenter(wxEvtHandler* parentHandler);
    ~AsyncAnalysisCenter();

    // 生产者接口：由 UI 线程调用
    void PushTask(const wxString& projectPath, const wxString& filePath);

protected:
    // 消费者核心：后台线程循环
    virtual wxThread::ExitCode Entry() override;

private:
    LintResult DoAnalysis(const wxString& path);

    TreeSitterResult TreeSitter(wxString code);
    SlangResult Slang(wxString code);

    TreeSitterResult TreeSitterFromPath(wxString filePath);
    SlangResult SlangFromPath(wxString projectPath);
    VerilatorResult Verilator(wxString path);
    

    wxEvtHandler* m_parentHandler;  // 接收事件的 UI 窗口句柄
    std::string m_projectPath;     // 待处理的代码缓冲区
    std::string m_pendingPath;     // 待处理的代码缓冲区
    bool m_hasNewTask;             // 任务标记
    std::mutex m_mutex;            // 保护缓冲区的互斥锁


    TSParser* parser = nullptr;
    TSTree* tree = nullptr;

    AtomAnalysis atomsAnalysis;
};
