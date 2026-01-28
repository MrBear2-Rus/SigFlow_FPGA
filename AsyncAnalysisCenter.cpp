#include "AsyncAnalysisCenter.h"
#include "LogicBridge.h"

#include <tree_sitter/api.h>
#include <json/json.h>
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

extern "C" TSLanguage* tree_sitter_verilog();

// 定义事件 ID
wxDEFINE_EVENT(EVT_ANALYSIS_COMPLETE, wxThreadEvent);

std::vector<int> GetStableLines(const std::vector<BlockInfo>& blocks);
std::vector<int> GetLineDepth(const std::vector<BlockInfo>& blocks, int total_lines);
std::vector<bool> isLineHeader(const std::vector<BlockInfo>& blocks, int total_lines);
std::vector<Stability> GetLineStatus(
    const std::vector<BlockInfo>& blocks,
    int code_line_count
);





AsyncAnalysisCenter::AsyncAnalysisCenter(wxEvtHandler* parentHandler)
    : m_parentHandler(parentHandler), m_hasNewTask(false)
{
}

AsyncAnalysisCenter::~AsyncAnalysisCenter() {

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
        wxString projectPath;
        wxString filePath;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            // 改进：使用条件变量代替 Sleep，提高响应速度
            if (!m_hasNewTask) {
                wxMilliSleep(50); // 或者使用 m_condition.wait_for
                continue;
            }
            projectPath = m_projectPath;
            filePath = m_pendingPath;
            m_hasNewTask = false;
        }

        std::string code;
        int line_count;
        wxFile file(filePath);
        if (file.IsOpened()) {
            wxString content;
            if (file.ReadAll(&content)) {
                code = content.ToStdString();

                // 关键：利用 wxString 的逻辑直接数行数
                // 这种方法会自动处理 \n, \r\n 等不同系统的换行符
                line_count = (int)content.Freq('\n') + 1;

                // 如果文件最后一行没换行，Freq 结果也是准的
            }
        }
        else {
            // 错误处理：文件打开失败
        }
        ///////////////////////////////////////////////////////////////////////////////////////
        sigTree = new SigFlowTree(projectPath.ToStdString());

        TSParser* parser = ts_parser_new();
        ts_parser_set_language(parser, tree_sitter_verilog());


        TSTree* new_tree = ts_parser_parse_string(parser, nullptr, code.c_str(), code.length());
        TSNode root = ts_tree_root_node(new_tree);
        TSTreeCursor cursor = ts_tree_cursor_new(root);
        std::string fp = filePath.ToStdString();
        sigTree->UpdateTreeFromTS(&cursor, sigTree->root, fp, code);
        sigTree->PrintTree();


        AnalysisResult res;

        // Tree-Sitter分析
        std::vector<BlockInfo> TSRes = TSLinter.LintFromPath(filePath);
        res.linted = true;
        res.lint.block_infos = TSRes;
        res.lint.is_lines_header = isLineHeader(TSRes, line_count);
        res.lint.line_depth = GetLineDepth(TSRes, line_count);
        res.lint.line_status = GetLineStatus(TSRes, line_count);
        res.lint.stable_lines = GetStableLines(TSRes);
        
        // 包装结果并推回 UI 线程
        wxThreadEvent* event = new wxThreadEvent(EVT_ANALYSIS_COMPLETE);
        event->SetPayload(res); // 现在 Payload 是 LintRes
        m_parentHandler->QueueEvent(event);





        // Slang分析
        std::unique_ptr<SlangProject> sp = LoadProject(projectPath);
        auto& driver = *sp->driver;


        // 注册 Driver 报错引擎 
        auto& engine = driver.diagEngine;

        class MyDiagClient : public slang::DiagnosticClient {
        public:
            void report(const slang::ReportedDiagnostic& diag) override {
                // 这里就是获取 driver parse 过程中所有错误的地方
                wxLogDebug("Real-time Error: %s", std::string(diag.formattedMessage));
            }
        };
        auto myClient = std::make_shared<MyDiagClient>();

        engine.clearClients();
        engine.addClient(myClient);

        // Slang Parse
        driver.parseAllSources();
        // Slang Logic Analysis
        const auto compilation = driver.createCompilation();

        res.schematic = LogicBridge::BuildSnapshot(*compilation);
        res.parsed = true;
        LogicBridge::printSnapshot(res.schematic);
        event = new wxThreadEvent(EVT_ANALYSIS_COMPLETE);
        event->SetPayload(res); // 现在 Payload 是 LintRes
        m_parentHandler->QueueEvent(event);



    }

    return (wxThread::ExitCode)0;
}







std::unique_ptr<SlangProject> AsyncAnalysisCenter::LoadProject(const wxString& projectPath) {
    auto sp = std::make_unique<SlangProject>();
    sp->driver = std::make_unique<slang::driver::Driver>();

    // --- 1. JSON 解析 (使用 JsonCpp) ---
    wxString fullPath = projectPath + wxFileName::GetPathSeparator() + "sigflow.project";
    wxFile file(fullPath);
    wxString content;
    file.ReadAll(&content);
    std::string utf8Content = content.ToUTF8().data();

    Json::Value root;
    Json::CharReaderBuilder builder;
    std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    std::string errs;

    if (!reader->parse(utf8Content.c_str(), utf8Content.c_str() + utf8Content.size(), &root, &errs)) {
        return nullptr;
    }

    // --- 2. 映射路径到 SourceLoader ---
    if (root["paths"].isMember("source_files")) {
        for (const auto& file : root["paths"]["source_files"]) {
            wxFileName fn1(file.asString());
            fn1.MakeAbsolute(projectPath);
            std::string absPath1 = fn1.GetFullPath().ToStdString();
            sp->persistentFiles.push_back(absPath1);
            sp->driver->sourceLoader.addFiles(sp->persistentFiles.back());
        }
    }

    // --- 3. 映射 Include 目录到 SourceManager ---
    if (root["paths"].isMember("include_dirs")) {
        for (const auto& dir : root["paths"]["include_dirs"]) {
            wxFileName fn2(dir.asString());
            fn2.MakeAbsolute(projectPath);
            std::string absPath2 = fn2.GetFullPath().ToStdString();
            sp->persistentIncludes.push_back(absPath2);
            sp->driver->sourceLoader.addSearchDirectories(sp->persistentIncludes.back());

        }
    }

    // --- 4. 映射 Build 配置到 Options ---
    if (root["build"].isMember("top_module")) {
        for (const auto& name : root["build"]["top_module"]) {
            sp->persistentTops.push_back(name.asString());
            sp->driver->options.topModules.push_back(sp->persistentTops.back());
        }
    }

    // --- 5. 执行流程 ---
    sp->driver->addStandardArgs(); // 初始化默认参数
    return sp;

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
                //OutputDebugStringA(wxString::Format("[%d] ", i));
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
        //OutputDebugStringA(wxString::Format("Line [%d]: Depth %d\n",i, depths[i]));
    }
    return depths;
}

std::vector<bool> isLineHeader(const std::vector<BlockInfo>& blocks, int total_lines) {
    // 1. 初始化差分数组
    std::vector<bool> mark(total_lines + 1, false);

    // 2. 每一个 Block 都是一个 [start, end] 的全覆盖区间
    for (const auto& b : blocks) {
        if (!(b.start_line == b.end_line)) mark[b.start_line] = true;
    }


    for (int i = 1; i <= total_lines; i++) {
        //OutputDebugStringA(wxString::Format("Line [%d]: Header? %d\n", i, mark[i]? 1:0));
    }
    return mark;
}

std::vector<Stability> GetLineStatus(
    const std::vector<BlockInfo>& blocks,
    int code_line_count
) {
    // 1. 初始化所有行为 Stable (长度设为 line_count + 1 以对齐行号)
    std::vector<Stability> status(code_line_count + 1, Stability::Stable);

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

    //printLineStatus(status);
    return status;
}
