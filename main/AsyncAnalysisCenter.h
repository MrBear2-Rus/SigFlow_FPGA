#pragma once
#include "TreeSitterLinter.h"
#include "SigTree.h"

#include <wx/wx.h>
#include <wx/thread.h>
#include <string>
#include <mutex>
#include <condition_variable>
#include <set>
#include <map>

#include <slang/driver/Driver.h>


struct LintResult {
    std::vector<BlockInfo> block_infos;
    std::vector<bool> is_lines_header;
    std::vector<int> line_depth;
    std::vector<Stability> line_status;
    std::vector<int> stable_lines;
};


struct SlangProject {
    std::unique_ptr<slang::driver::Driver> driver;
    // 这里的 vector 会跟随 bundle 一起存在，保证内存不过期
    std::vector<std::string> persistentFiles;
    std::vector<std::string> persistentIncludes;
    std::vector<std::string> persistentTops;
};


struct AnalysisResult {
    bool linted;
    LintResult lint;
};




// --- 自定义事件定义 ---
wxDECLARE_EVENT(EVT_ANALYSIS_COMPLETE, wxThreadEvent);

class AsyncAnalysisCenter : public wxThreadHelper {
public:
    AsyncAnalysisCenter(wxEvtHandler* parentHandler);
    ~AsyncAnalysisCenter();

    // 生产者接口：由 UI 线程调用
    void PushTask(const wxString& projectPath, const wxString& code);

protected:
    // 消费者核心：后台线程循环
    virtual wxThread::ExitCode Entry() override;

private:
    std::unique_ptr<SlangProject> LoadProject(const wxString& projectPath);


    

    wxEvtHandler* m_parentHandler;  // 接收事件的 UI 窗口句柄
    // 用 wxString 保存：赋给 std::string 会走 locale 转换（Windows 上变 ANSI），
    // 路径/代码里的非 ASCII 字符会被破坏。真正需要字节时再显式 ToUTF8()。
    wxString m_projectPath;        // 待处理的项目路径
    wxString m_pendingCode;        // 待处理的代码缓冲区
    bool m_hasNewTask = false;     // 任务标记
    bool m_shutdown = false;       // 请求后台线程退出
    std::mutex m_mutex;            // 保护缓冲区的互斥锁
    std::condition_variable m_condition;  // 取代"持锁 sleep 50ms"的忙等

    TreeSitterLinter TSLinter;
    SigFlowTree* sigTree;

};
