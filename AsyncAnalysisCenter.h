#pragma once
#include "TreeSitterLinter.h"

#include <wx/wx.h>
#include <wx/thread.h>
#include <string>
#include <mutex>
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
    std::unique_ptr<SlangProject> LoadProject(const wxString& projectPath);


    

    wxEvtHandler* m_parentHandler;  // 接收事件的 UI 窗口句柄
    std::string m_projectPath;     // 待处理的代码缓冲区
    std::string m_pendingPath;     // 待处理的代码缓冲区
    bool m_hasNewTask;             // 任务标记
    std::mutex m_mutex;            // 保护缓冲区的互斥锁

    TreeSitterLinter TSLinter;

};
