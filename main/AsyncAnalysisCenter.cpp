#include "AsyncAnalysisCenter.h"
#include "platform/PlatformPaths.h"

#include <tree_sitter/api.h>
#include <json/json.h>
#include <regex>
#include <wx/file.h>
#include <wx/process.h>
#include <wx/txtstrm.h>
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
    // 请求后台线程退出并等待它真正返回。
    //
    // 旧实现：析构为空 + wxTHREAD_DETACHED，TestDestroy() 永远不会为真，
    // 只能靠 wxThreadHelper 析构里的 KillThread()/wxThread::Kill() 强杀线程，
    // 有可能在它持有互斥锁时把线程打死（留下永久锁死的 mutex）。
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_shutdown = true;
    }
    m_condition.notify_all();
    if (wxThread* thread = GetThread()) {
        thread->Delete();   // joinable 线程：等待 Entry() 返回
    }
}

void AsyncAnalysisCenter::PushTask(const wxString& projectPath, const wxString& code) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_projectPath = projectPath;
        m_pendingCode = code;
        m_hasNewTask = true;
    }
    m_condition.notify_one();   // 叫醒等待中的消费者，不必再等 50ms

    wxThread* thread = GetThread();
    if (!thread || !thread->IsRunning()) {
        // JOINABLE（而不是 DETACHED），这样析构时可以优雅地等待线程结束。
        if (CreateThread(wxTHREAD_JOINABLE) == wxTHREAD_NO_ERROR) {
            GetThread()->Run();
        }
    }
}

wxThread::ExitCode AsyncAnalysisCenter::Entry() {
    while (!GetThread()->TestDestroy()) {
        wxString projectPath;
        wxString code;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            // 条件变量等待：绝不能在持锁状态下 sleep。
            // 旧实现 wxMilliSleep(50) 是在 lock 作用域内执行的，
            // 于是 UI 线程每次 PushTask 都可能被阻塞最长 50ms。
            m_condition.wait(lock, [this] { return m_hasNewTask || m_shutdown; });
            if (m_shutdown) {
                break;
            }
            projectPath = m_projectPath;
            code = m_pendingCode;
            m_hasNewTask = false;
        }

        const std::size_t line_count = static_cast<std::size_t>(
            std::count(code.begin(), code.end(), '\n'));

        // 如果最后一行没有以 \n 结尾，通常也算作一行。
        // 注意必须先判空：wxString::Last() 对空串会断言/解引用无效迭代器。
        std::size_t total_lines = line_count;
        if (!code.IsEmpty() && code.Last() != '\n') {
            total_lines++;
        }
        (void)total_lines;

        ///////////////////////////////////////////////////////////////////////////////////////
        //sigTree = new SigFlowTree(projectPath.ToStdString());

        TSParser* parser = ts_parser_new();
        ts_parser_set_language(parser, tree_sitter_verilog());

        /*
        TSTree* new_tree = ts_parser_parse_string(parser, nullptr, code.c_str(), code.length());
        TSNode root = ts_tree_root_node(new_tree);
        TSTreeCursor cursor = ts_tree_cursor_new(root);
        std::string fp = filePath.ToStdString();
        sigTree->UpdateTreeFromTS(&cursor, sigTree->root, fp, code);
        sigTree->PrintTree();
        //char* tree_str = ts_node_string(root);
        //wxLogDebug(tree_str);
        //free(tree_str);

        AnalysisResult res;

        // Tree-Sitter分析
        std::vector<BlockInfo> TSRes = TSLinter.LintFromPath(filePath);
        res.linted = true;
        res.lint.block_infos = TSRes;
        res.lint.is_lines_header = isLineHeader(TSRes, line_count);
        res.lint.line_depth = GetLineDepth(TSRes, line_count);
        res.lint.line_status = GetLineStatus(TSRes, line_count);
        
        // 包装结果并推回 UI 线程
        wxThreadEvent* event = new wxThreadEvent(EVT_ANALYSIS_COMPLETE);
        event->SetPayload(res); // 现在 Payload 是 LintRes
        m_parentHandler->QueueEvent(event);

        */
        /*


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


        */


        // 旧实现从不释放 parser：每次任务泄漏一个原生 TSParser（无界内存增长）。
        // 上面被注释掉的分析逻辑若将来恢复，请把 ts_parser_delete 放在所有 return 之前。
        ts_parser_delete(parser);
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
    if (!file.IsOpened() || !file.ReadAll(&content)) {
        return nullptr;   // 旧实现忽略返回值，会拿空串去解析 JSON
    }
    std::string utf8Content = sigflow::platform::Utf8String(content);

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
            std::string absPath1 = sigflow::platform::Utf8String(fn1.GetFullPath());
            sp->persistentFiles.push_back(absPath1);
            sp->driver->sourceLoader.addFiles(sp->persistentFiles.back());
        }
    }

    // --- 3. 映射 Include 目录到 SourceManager ---
    if (root["paths"].isMember("include_dirs")) {
        for (const auto& dir : root["paths"]["include_dirs"]) {
            wxFileName fn2(dir.asString());
            fn2.MakeAbsolute(projectPath);
            std::string absPath2 = sigflow::platform::Utf8String(fn2.GetFullPath());
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
