#include "pch.h"
#include "Plug_DeepSeek.h"
#include <wx/clipbrd.h>  // <--- 新增这一行：剪贴板支持库
#include <wx/statline.h>
#include <iostream>
#include <winhttp.h>
#include <nlohmann/json.hpp> // 需要安装 json 库
#include <Windows.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <set>
#include <vector>
#include <map>
#include <chrono>
#include <iomanip>
#include <cstdlib>
#include <functional>
#include <atomic>
#include <mutex>

#pragma comment(lib, "winhttp.lib") // 告诉编译器自动链接 winhttp 库
using json = nlohmann::json;
struct DSResult {
    wxString analysis;  // 1. 逻辑分析过程
    wxString code;      // 2. 提取出的纯 Verilog 代码
    wxString summary;   // 3. 改动简要总结
    wxString memory;    // 4. 待存储的长期记忆点
};

DSResult ParseDSResponse(const wxString& raw);

// Helper: create safe filename from raw token
static std::string MakeSafeFilename(const std::string& raw, const std::string& defaultExt = ".v") {
    if (raw.empty()) return std::string("untitled") + defaultExt;
    // remove surrounding whitespace
    size_t s = 0, e = raw.size();
    while (s < e && isspace((unsigned char)raw[s])) ++s;
    while (e > s && isspace((unsigned char)raw[e-1])) --e;
    std::string name = raw.substr(s, e - s);

    // strip surrounding quotes/backticks
    if ((name.size() >= 2) && ((name.front() == '"' && name.back() == '"') || (name.front() == '\'' && name.back() == '\'' ) || (name.front() == '`' && name.back() == '`'))) {
        name = name.substr(1, name.size()-2);
    }

    // if contains path separators, return cleaned basename (caller should preserve parent path separately)
    for (auto &c : name) if (c == '\\') c = '/';
    size_t lastSlash = name.find_last_of('/');
    std::string base = (lastSlash == std::string::npos) ? name : name.substr(lastSlash + 1);

    // remove illegal chars and convert CamelCase to lower_with_underscore heuristically
    std::string out;
    for (size_t i = 0; i < base.size(); ++i) {
        char c = base[i];
        if (isalnum((unsigned char)c) || c == '_' || c == '-' || c == '$' || c == '.') {
            out.push_back(c);
        } else if (isspace((unsigned char)c) || c == ':' || c == '/') {
            out.push_back('_');
        } else {
            // replace other punctuation
            out.push_back('_');
        }
    }

    if (out.empty()) out = "untitled";

    // If starts with digit, prefix
    if (isdigit((unsigned char)out[0])) out = std::string("m_") + out;

    // ensure lower-case (except extension)
    size_t dot = out.find_last_of('.');
    std::string nameNoExt = (dot == std::string::npos) ? out : out.substr(0, dot);
    std::string ext = (dot == std::string::npos) ? std::string() : out.substr(dot);
    for (auto &c : nameNoExt) if (isupper((unsigned char)c)) c = (char)tolower(c);
    for (auto &c : ext) if (isupper((unsigned char)c)) c = (char)tolower(c);

    if (ext.empty()) ext = defaultExt;

    return nameNoExt + ext;
}

// 查找解决方案/仓库根目录：从当前工作目录向上查找第一个包含 `.sln` 的目录，找不到则返回当前工作目录
static std::string FindSolutionRoot() {
    namespace fs = std::filesystem;
    fs::path p = fs::current_path();
    while (true) {
        try {
                for (auto &entry : fs::directory_iterator(p)) {
                // 1) 传统的 Visual Studio 解决方案
                if (entry.is_regular_file() && entry.path().extension() == ".sln") return p.string();

                // 2) sigflow 项目标记文件
                if (entry.is_regular_file() && entry.path().filename() == "sigflow.project") return p.string();

                // 3) .sigflow 工作区目录
                if (entry.is_directory() && entry.path().filename() == ".sigflow") return p.string();
            }

        
        }
        catch (...) {
            // 忽略不可访问的目录
        }

        if (p.has_parent_path()) p = p.parent_path();
        else break;
    }
    return std::filesystem::current_path().string();
}

// 遍历项目文件并汇总为一个文本块，包含文件名和文件内容。为避免过大，会限制最大字符数和文件数量。
static std::string GatherProjectFiles(const std::string& rootPath, size_t maxTotalChars = 150000, size_t maxFiles = 200) {
    namespace fs = std::filesystem;
    std::string result;
    size_t total = 0;
    size_t count = 0;

    std::set<std::string> ignoreDirs = {".git", "build", "bin", "obj", ".vs", "Debug", "Release"};
    std::set<std::string> allowedExt = {".cpp",".c",".h",".hpp",".txt",".md",".py",".cs",".java",".json",".xml",".sln",".vcxproj",".vcxproj.filters",".rc",".yml",".yaml",".ini",".cmake",".makefile",".pro",".project",".v",".sv",".vh",".svh"};

    try {
        for (auto it = fs::recursive_directory_iterator(rootPath); it != fs::recursive_directory_iterator(); ++it) {
            if (count >= maxFiles || total >= maxTotalChars) break;

            try {
                const auto& p = it->path();
                if (p.has_filename()) {
                    std::string fname = p.filename().string();
                    // 跳过忽略目录
                    for (const auto& ig : ignoreDirs) {
                        if (fname == ig) { it.disable_recursion_pending(); goto cont; }
                    }
                }

                if (!it->is_regular_file()) { cont: ; continue; }

                std::string ext = p.extension().string();
                // 小写扩展名
                for (auto &ch : ext) ch = (char)tolower(ch);

                if (allowedExt.find(ext) == allowedExt.end()) continue;

                std::ifstream ifs(p, std::ios::in | std::ios::binary);
                if (!ifs) continue;
                std::stringstream ss;
                ss << ifs.rdbuf();
                std::string content = ss.str();

                // 限制单文件大小以避免超长
                const size_t maxPerFile = 20000;
                if (content.size() > maxPerFile) content = content.substr(0, maxPerFile) + "\n...<truncated>...\n";

                std::string header = "==== " + p.string() + " ====\n";
                if (total + header.size() + content.size() > maxTotalChars) break;

                result += header;
                result += content + "\n\n";

                total += header.size() + content.size();
                ++count;
            }
            catch (...) { continue; }
        }
    }
    catch (...) { /* 忽略遍历异常 */ }

    if (result.empty()) result = "<No project files collected>";
    return result;
}

// 在 rootPath 下查找首个包含 Verilog 文件（.v/.sv/.vh/.svh）的子目录，优先返回较浅的目录。
static std::string FindVerilogSubdir(const std::string& rootPath) {
    namespace fs = std::filesystem;
    std::vector<std::string> verilogExts = {".v", ".sv", ".vh", ".svh"};
    try {
        for (auto it = fs::recursive_directory_iterator(rootPath); it != fs::recursive_directory_iterator(); ++it) {
            try {
                if (!it->is_directory()) continue;
                const auto dir = it->path();
                size_t found = 0;
                for (auto& entry : fs::directory_iterator(dir)) {
                    if (!entry.is_regular_file()) continue;
                    std::string ext = entry.path().extension().string();
                    for (auto &ch : ext) ch = (char)tolower(ch);
                    for (const auto& ve : verilogExts) {
                        if (ext == ve) { ++found; break; }
                    }
                    if (found) break;
                }
                if (found) return dir.string();
            }
            catch (...) { continue; }
        }
    }
    catch (...) { }
    return std::string();
}








wxDEFINE_EVENT(EVT_AI_RESPONSE, wxThreadEvent);

Plug_DeepSeek::Plug_DeepSeek() {
    m_apiKey = "sk-8801be45326a4776ac37f3b120ee1888"; // 实际开发建议从配置文件读取
    m_apiUrl = "https://api.deepseek.com/chat/completions";
    m_projectRoot = "";

    // 设计决策：将历史对话保存在当前用户的 Local AppData 下的插件目录中，
    // 这样无需管理员权限且对多用户环境友好。
    const char* localApp = std::getenv("LOCALAPPDATA");
    if (localApp && localApp[0] != '\0') {
        m_dataDir = std::string(localApp) + "\\SuperEDA\\DeepSeekPlugin";
    }
    else {
        // 回退到当前可写目录
        m_dataDir = std::filesystem::current_path().string() + "\\DeepSeekPluginData";
    }
    m_historyFile = m_dataDir + "\\history.json";

    // 尝试加载已有的历史对话
    try {
        // create dir if needed
        std::error_code ec;
        std::filesystem::create_directories(m_dataDir, ec);
        if (!ec) {
            std::ifstream ifs(m_historyFile);
            if (ifs) {
                nlohmann::json j;
                ifs >> j;
                if (j.is_array()) {
                    for (auto &it : j) {
                        if (it.contains("name") && it.contains("content")) {
                            std::string name = it["name"].get<std::string>();
                            std::string content = it["content"].get<std::string>();
                            m_savedConversations.push_back(name);
                            m_conversationContents[name] = content;
                        }
                    }
                }
            }
        }
    }
    catch (...) { /* 忽略加载异常 */ }

}

// 将当前内存的会话列表写入磁盘
void Plug_DeepSeek::SaveConversationsToDisk() {
    try {
        nlohmann::json j = nlohmann::json::array();
        for (const auto& name : m_savedConversations) {
            nlohmann::json it;
            it["name"] = name;
            auto cit = m_conversationContents.find(name);
            if (cit != m_conversationContents.end()) it["content"] = cit->second;
            else it["content"] = "";
            j.push_back(it);
        }

        std::ofstream ofs(m_historyFile, std::ios::trunc);
        if (ofs) ofs << j.dump(2);
    }
    catch (...) { /* 忽略写盘错误 */ }
}

void Plug_DeepSeek::AddConversation(const std::string& name, const std::string& content) {
    // 保持简单：如果已存在同名会话，追加索引
    std::string finalName = name;
    int idx = 1;
    while (m_conversationContents.find(finalName) != m_conversationContents.end()) {
        finalName = name + " (" + std::to_string(idx++) + ")";
    }
    m_savedConversations.push_back(finalName);
    m_conversationContents[finalName] = content;
    SaveConversationsToDisk();
}

void Plug_DeepSeek::RemoveConversation(const std::string& name) {
    auto it = std::find(m_savedConversations.begin(), m_savedConversations.end(), name);
    if (it != m_savedConversations.end()) m_savedConversations.erase(it);
    m_conversationContents.erase(name);
    SaveConversationsToDisk();
}

void Plug_DeepSeek::RenameConversation(const std::string& oldName, const std::string& newName) {
    if (oldName == newName) return;
    // ensure newName doesn't collide
    std::string finalName = newName;
    int idx = 1;
    while (m_conversationContents.find(finalName) != m_conversationContents.end()) {
        finalName = newName + " (" + std::to_string(idx++) + ")";
    }

    auto it = m_conversationContents.find(oldName);
    if (it == m_conversationContents.end()) return;
    std::string content = it->second;
    m_conversationContents.erase(it);
    m_conversationContents[finalName] = content;

    // replace in vector
    for (auto &n : m_savedConversations) {
        if (n == oldName) { n = finalName; break; }
    }

    // adjust current session name if needed
    if (m_currentSessionName == oldName) m_currentSessionName = finalName;

    SaveConversationsToDisk();
}

void Plug_DeepSeek::ExportConversation(const std::string& name, const std::string& path) {
    auto it = m_conversationContents.find(name);
    if (it == m_conversationContents.end()) return;
    try {
        std::ofstream ofs(path, std::ios::binary);
        if (ofs) ofs << it->second;
    }
    catch (...) { }
}

bool Plug_DeepSeek::LoadConversationIntoSession(const std::string& name) {
    auto it = m_conversationContents.find(name);
    if (it == m_conversationContents.end()) return false;
    m_currentSessionName = name;
    m_currentSessionHistory = it->second;
    return true;
}

// 接收宿主传入的项目根路径
void Plug_DeepSeek::SetProjectRoot(const std::string& path) {
    if (!path.empty()) m_projectRoot = path;
}

Plug_DeepSeek::~Plug_DeepSeek() {}

std::string Plug_DeepSeek::GetName() const {
    return "DeepSeek_Assistant";
}

void Plug_DeepSeek::Release() {
    m_isReleased = true; // 告诉所有线程别干了

    // 等待所有正在运行的后台线程结束
    for (auto& t : m_threads) {
        if (t.joinable()) {
            t.join();
        }
    }

    delete this; // 安全自毁
}


std::string Plug_DeepSeek::ProcessCommand(const std::string& cmd) {
    // 修复编码隐患：必须用 ToUTF8() 转换为标准 UTF-8 字节流，切忌使用 ToStdString()
    std::string memStr = memory.IsEmpty() ? "" : std::string(memory.ToUTF8().data());

    // 支持特殊命令：/scanproject 或 /scan 来收集本仓库/解决方案下的所有文本源码文件并发送给 AI
    // 新增命令：/autogen 用于自动生成单文件或多文件 Verilog/相关源码，并写入到 projectRoot/src/ 或 projectRoot/lib/
    // 兼容旧命令 /genfile
    // 语法示例：
    //   /autogen <optional-filename> ;; <prompt>
    // 如果任务需要多个文件，AI 在回复的 "## 2. 纯代码" 部分应以如下可解析格式输出多文件：
    // ==== path/to/file1.v ====
    // <file1 content>
    // ==== path/to/lib/header.svh ====
    // <file2 content>
    // 否则，单文件情况下直接返回代码块。
    const std::string genCmd = "/autogen";
    const std::string legacyGenCmd = "/genfile"; // 兼容
    std::string fullPrompt;
    const std::string scanCmd = "/scanproject";
    const std::string scanCmd2 = "/scan";

    // 处理 /autogen 或 兼容 /genfile 命令（优先于 /scan）
    if (cmd.rfind(genCmd, 0) == 0 || cmd.rfind(legacyGenCmd, 0) == 0) {
        // 解析可选的文件名与提示（使用双分号 ';;' 分隔）
        std::string userInput;
        size_t pos = cmd.find(' ');
        std::string filename;
        std::string userQuestion = "请生成符合 Verilog 语法、可综合的模块实现。";
        if (pos != std::string::npos) {
            userInput = cmd.substr(pos + 1);
            size_t sep = userInput.find(";;");
            if (sep != std::string::npos) {
                filename = userInput.substr(0, sep);
                while (!filename.empty() && isspace((unsigned char)filename.back())) filename.pop_back();
                userQuestion = userInput.substr(sep + 2);
                while (!userQuestion.empty() && isspace((unsigned char)userQuestion.front())) userQuestion.erase(userQuestion.begin());
            }
            else {
                // 如果只有一部分，视为问题文本
                userQuestion = userInput;
            }
        }

        // 设置自动创建标志与可选文件名；允许多文件解析
        this->m_autoCreatePending = true;
        this->m_autoFilename = filename;
        this->m_autoAllowMulti = true;

        // 构造提示，要求 AI 在需要多文件时使用可解析的分隔格式
        fullPrompt = memStr +
            "你是一个严格遵守格式的 Verilog/SystemVerilog 专家。无论用户问什么，你都必须且只能按以下格式回复，严禁任何前言和后语：\n"
            "## 1. 分析\n...\n"
            "## 2. 纯代码\n(如果需要多个文件，请在这里以以下格式输出多文件内容：\n"
            "==== path/to/file1.v ====\n<file1 content>\n==== path/to/lib/header.svh ====\n<file2 content>\n)\n"
            "## 3. 简要总结\n...\n"
            "## 4. 记忆存储\n...\n\n"
            "现在开始！用户的请求是：" + userQuestion;
    }
    else if (cmd.rfind(scanCmd, 0) == 0 || cmd.rfind(scanCmd2, 0) == 0) {
        // 从命令中提取后续用户问题（空格后的部分）
        std::string userQuestion;
        size_t pos = cmd.find(' ');
        std::string userPath;
        if (pos != std::string::npos) {
            userQuestion = cmd.substr(pos + 1);
            // 如果用户同时指定了路径和问题，支持格式：/scan <path> ;; <question>
            // 用双分号分隔路径与问题（简单解析）
            size_t sep = userQuestion.find(";;");
            if (sep != std::string::npos) {
                userPath = userQuestion.substr(0, sep);
                // 去掉可能的空格
                while (!userPath.empty() && isspace((unsigned char)userPath.back())) userPath.pop_back();
                // 剩余为实际问题
                userQuestion = userQuestion.substr(sep + 2);
                while (!userQuestion.empty() && isspace((unsigned char)userQuestion.front())) userQuestion.erase(userQuestion.begin());
            }
        }
        else userQuestion = "请基于项目内容回答用户的问题。";

        // 找到仓库/解决方案根目录
        // 优先使用宿主传入的项目路径（由主程序在打开项目时提供），否则回退到自动搜索
        std::string root;
        if (!m_projectRoot.empty()) root = m_projectRoot;
        else root = FindSolutionRoot();
        std::string targetRoot;

        if (!userPath.empty()) {
            // 如果用户指定了路径，支持相对路径（相对于 solution root）或绝对路径
            namespace fs = std::filesystem;
            fs::path p(userPath);
            if (p.is_relative()) p = fs::path(root) / p;
            if (fs::exists(p) && fs::is_directory(p)) targetRoot = p.string();
        }

        // 如果没有用户路径，尝试智能定位 Verilog 源码目录
        if (targetRoot.empty()) {
            std::string verDir = FindVerilogSubdir(root);
            if (!verDir.empty()) targetRoot = verDir;
            else targetRoot = root; // 回退到整个解决方案根
        }

        std::string projectFiles = GatherProjectFiles(targetRoot);

        fullPrompt = memStr + "下面是项目中收集到的文件内容（已做截断以避免过大）:\n" + projectFiles + "\n";
        fullPrompt += "你是一个项目分析专家。请基于上面提供的项目内容回答用户的问题（不要添加与项目无关的内容）。用户的问题：" + userQuestion;
    }
    else {
        // 只要配置了 /utf-8 编译项，这里的双引号中文就是安全的 UTF-8
        fullPrompt = memStr +
            "你是一个严格遵守格式的 Verilog 专家。无论用户问什么，你都必须且只能按以下格式回复，严禁任何前言和后语：\n"
            "## 1. 分析\n...\n"
            "## 2. 纯代码\n...\n"
            "## 3. 简要总结\n...\n"
            "## 4. 记忆存储\n...\n\n"
            "现在开始！用户的请求是：" + cmd;
    }

    // 如果存在已设置的 panel，则启用流式回传，便于增量显示
    return CallDeepSeekAPI(fullPrompt, this->m_panel, true);
}
std::string Plug_DeepSeek::CallDeepSeekAPI(const std::string& prompt, wxWindow* panel, bool stream) {
    std::string responseData;
    HINTERNET hSession = NULL, hConnect = NULL, hRequest = NULL;

    // 简短的重试策略参数
    const int maxRetries = 3;
    int attempt = 0;

    // 标记请求开始
    m_requestInProgress = true;
    m_cancelRequest = false;

    while (attempt < maxRetries && !m_cancelRequest) {
        ++attempt;

        // 1. 初始化 WinHTTP
        hSession = WinHttpOpen(L"EDA Assistant/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!hSession) {
            responseData = "Error: WinHttpOpen failed.";
            break;
        }
        WinHttpSetTimeouts(hSession, 60000, 60000, 60000, 120000);

        // 快照句柄以便取消
        {
            std::lock_guard<std::mutex> lk(m_requestMutex);
            m_hSessionHandle = hSession;
        }

        // 2. 指定服务器
        hConnect = WinHttpConnect(hSession, L"api.deepseek.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
        {
            std::lock_guard<std::mutex> lk(m_requestMutex);
            m_hConnectHandle = hConnect;
        }

        if (!hConnect) {
            responseData = "Error: WinHttpConnect failed.";
            WinHttpCloseHandle(hSession);
            continue;
        }

        // 3. 创建请求
        hRequest = WinHttpOpenRequest(hConnect, L"POST", L"/chat/completions", NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
        {
            std::lock_guard<std::mutex> lk(m_requestMutex);
            m_hRequestHandle = hRequest;
        }

        if (!hRequest) {
            responseData = "Error: WinHttpOpenRequest failed.";
            WinHttpCloseHandle(hConnect);
            WinHttpCloseHandle(hSession);
            continue;
        }

        // 4. 构造 Payload 和 Header
        json payload = {
            {"model", "deepseek-chat"},
            {"messages", {{{"role", "user"}, {"content", prompt}}}},
            {"temperature", 0.0},
            {"stream", stream}
        };
        std::string jsonStr = payload.dump();

        std::wstring wKey(m_apiKey.begin(), m_apiKey.end());
        std::wstring headers = L"Content-Type: application/json\r\nAuthorization: Bearer " + wKey + L"\r\n";

        BOOL bResults = WinHttpSendRequest(hRequest, headers.c_str(), (DWORD)-1L, (LPVOID)jsonStr.c_str(), (DWORD)jsonStr.length(), (DWORD)jsonStr.length(), 0);
        if (!bResults) {
            responseData = "Error: WinHttpSendRequest failed.";
            // cleanup and maybe retry
        } else {
            bResults = WinHttpReceiveResponse(hRequest, NULL);
        }

        // 检查 HTTP 状态码
        DWORD statusCode = 0;
        DWORD statusSize = sizeof(statusCode);
        if (hRequest && WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusSize, WINHTTP_NO_HEADER_INDEX)) {
            // statusCode now contains numeric HTTP status
        }

        // handle auth error / rate limit / server errors
        if (statusCode == 401) {
            responseData = "DeepSeek API Error: Unauthorized (401). Check API key.";
            // don't retry
            bResults = FALSE;
        }

        if (!bResults) {
            // gather WinHTTP error if available
            if (responseData.empty()) responseData = "Error: Network request failed.";
        }

        if (bResults && stream && panel) {
            // 流式读取并增量回传
            DWORD dwSize = 0;
            std::string sseBuf;
            std::string assembledContent;
            do {
                if (m_cancelRequest) break;
                if (!WinHttpQueryDataAvailable(hRequest, &dwSize)) break;
                if (dwSize == 0) break;

                std::vector<char> buffer(dwSize + 1);
                DWORD dwDownloaded = 0;
                if (WinHttpReadData(hRequest, buffer.data(), dwSize, &dwDownloaded) && dwDownloaded > 0) {
                    // accumulate raw
                    responseData.append(buffer.data(), dwDownloaded);
                    // append to sse buffer for event parsing
                    sseBuf.append(buffer.data(), dwDownloaded);

                    // process complete SSE events separated by "\n\n"
                    size_t pos = 0;
                    while ((pos = sseBuf.find("\n\n")) != std::string::npos) {
                        std::string event = sseBuf.substr(0, pos);
                        sseBuf.erase(0, pos + 2);

                        // extract lines that start with "data:"
                        std::istringstream iss(event);
                        std::string line;
                        std::string dataStr;
                        while (std::getline(iss, line)) {
                            if (line.rfind("data:", 0) == 0) {
                                std::string d = line.substr(5);
                                if (!d.empty() && d[0] == ' ') d.erase(0, 1);
                                dataStr += d;
                            }
                        }

                        if (dataStr.empty()) continue;
                        if (dataStr == "[DONE]") {
                            // stream finished marker
                            continue;
                        }

                        // try parse JSON and extract delta.content
                        try {
                            auto j = json::parse(dataStr);
                            if (j.contains("choices") && j["choices"].is_array() && !j["choices"].empty()) {
                                auto& ch = j["choices"][0];
                                // prefer delta.content (stream)
                                if (ch.contains("delta") && ch["delta"].contains("content")) {
                                    std::string delta = ch["delta"]["content"].get<std::string>();
                                    assembledContent += delta;
                                    wxThreadEvent* partEvt = new wxThreadEvent(EVT_AI_RESPONSE);
                                    partEvt->SetString(wxString::FromUTF8(delta));
                                    partEvt->SetInt(1);
                                    wxQueueEvent(panel, partEvt);
                                }
                                else if (ch.contains("message") && ch["message"].contains("content")) {
                                    std::string content = ch["message"]["content"].get<std::string>();
                                    assembledContent += content;
                                    wxThreadEvent* partEvt = new wxThreadEvent(EVT_AI_RESPONSE);
                                    partEvt->SetString(wxString::FromUTF8(content));
                                    partEvt->SetInt(1);
                                    wxQueueEvent(panel, partEvt);
                                }
                            }
                        }
                        catch (...) {
                            // ignore malformed event
                        }
                    }
                }
            } while (dwSize > 0 && !m_cancelRequest);

            // 最终事件（包含完整解析出的内容或回退到原始响应）
            if (!m_cancelRequest) {
                std::string finalStr = !assembledContent.empty() ? assembledContent : responseData;
                // if finalStr looks like JSON, try extract message.content as fallback
                try {
                    auto resJson = json::parse(responseData);
                    if (resJson.contains("choices") && resJson["choices"].is_array() && !resJson["choices"].empty()) {
                        auto& firstChoice = resJson["choices"][0];
                        if (firstChoice.contains("message") && firstChoice["message"].contains("content")) {
                            finalStr = firstChoice["message"]["content"].get<std::string>();
                        }
                    }
                } catch (...) { /* ignore */ }

                wxThreadEvent* finalEvt = new wxThreadEvent(EVT_AI_RESPONSE);
                finalEvt->SetString(wxString::FromUTF8(finalStr));
                finalEvt->SetInt(2);
                wxQueueEvent(panel, finalEvt);
            } else {
                // cancellation notification
                wxThreadEvent* cancelEvt = new wxThreadEvent(EVT_AI_RESPONSE);
                cancelEvt->SetString(wxString::FromUTF8("Error: Request cancelled by user."));
                cancelEvt->SetInt(3);
                wxQueueEvent(panel, cancelEvt);
            }

        } else if (bResults) {
            // 非流式：一次性读取全部
            DWORD dwSize = 0;
            do {
                if (!WinHttpQueryDataAvailable(hRequest, &dwSize)) break;
                if (dwSize == 0) break;

                std::vector<char> buffer(dwSize + 1);
                DWORD dwDownloaded = 0;
                if (WinHttpReadData(hRequest, buffer.data(), dwSize, &dwDownloaded) && dwDownloaded > 0) {
                    responseData.append(buffer.data(), dwDownloaded);
                }
            } while (dwSize > 0);
        }

        // 清理本次请求句柄快照
        {
            std::lock_guard<std::mutex> lk(m_requestMutex);
            m_hRequestHandle = NULL;
            m_hConnectHandle = NULL;
            m_hSessionHandle = NULL;
        }

        if (hRequest) WinHttpCloseHandle(hRequest);
        if (hConnect) WinHttpCloseHandle(hConnect);
        if (hSession) WinHttpCloseHandle(hSession);

        // 如果状态码为 429 或 5xx，允许重试（带指数退避）
        if (statusCode == 429 || (statusCode >= 500 && statusCode < 600)) {
            if (attempt < maxRetries && !m_cancelRequest) {
                int backoffMs = 500 * (1 << (attempt - 1));
                Sleep(backoffMs);
                responseData.clear();
                continue; // retry
            }
        }

        break; // exit retry loop
    }

    m_requestInProgress = false;

    if (m_cancelRequest) return std::string("Error: Request cancelled by user.");

    if (responseData.empty()) return std::string("Error: No data from API.");

    // 解析并返回（非流式或最终返回）
    try {
        auto resJson = json::parse(responseData);

        if (resJson.contains("error")) {
            return "DeepSeek API Error: " + resJson["error"]["message"].get<std::string>();
        }

        if (resJson.contains("choices") && resJson["choices"].is_array() && !resJson["choices"].empty()) {
            auto& firstChoice = resJson["choices"][0];
            if (firstChoice.contains("message") && firstChoice["message"].contains("content")) {
                return firstChoice["message"]["content"].get<std::string>();
            }
        }

        return "Error: Unexpected JSON format. Raw Response: " + responseData;
    }
    catch (const json::exception& e) {
        return "JSON Error: " + std::string(e.what()) + "\nRaw data: " + responseData;
    }
    catch (...) {
        return "Critical Error: An unknown exception occurred during parsing.";
    }
}

void Plug_DeepSeek::CancelCurrentRequest() {
    m_cancelRequest = true;
    std::lock_guard<std::mutex> lk(m_requestMutex);
    if (m_hRequestHandle) {
        // Closing the request handle should interrupt ongoing WinHttpReadData/Query operations
        WinHttpCloseHandle(m_hRequestHandle);
        m_hRequestHandle = NULL;
    }
    if (m_hConnectHandle) {
        WinHttpCloseHandle(m_hConnectHandle);
        m_hConnectHandle = NULL;
    }
    if (m_hSessionHandle) {
        WinHttpCloseHandle(m_hSessionHandle);
        m_hSessionHandle = NULL;
    }
}

// 在 Plug_DeepSeek.cpp 中
extern "C" __declspec(dllexport) ISigPlugin* CreateSigPlugin() {
    return new Plug_DeepSeek();
}

wxPanel* Plug_DeepSeek::CreatePanel(wxWindow* parent) {
    // 1. 创建主面板
    wxPanel* panel = new wxPanel(parent, wxID_ANY);
    // 保存 panel 指针以便网络代码可以回传流
    this->m_panel = panel;
    panel->SetBackgroundColour(wxColour(245, 245, 245)); // 浅灰色背景

    // 2. 创建控件
    // 历史对话框 (只读)
    wxTextCtrl* historyCtrl = new wxTextCtrl(panel, wxID_ANY, wxEmptyString,
        wxDefaultPosition, wxDefaultSize, wxTE_MULTILINE | wxTE_READONLY | wxTE_RICH2);

    // 输入框 (支持回车发送)
    wxTextCtrl* inputCtrl = new wxTextCtrl(panel, wxID_ANY, wxEmptyString,
        wxDefaultPosition, wxDefaultSize, wxTE_PROCESS_ENTER);
    inputCtrl->SetHint(wxString::FromUTF8("输入问题，按回车或点击发送..."));

    // --- 修改区：增加复制按钮，调整按钮宽度 ---
    wxButton* copyBtn = new wxButton(panel, wxID_ANY, wxString::FromUTF8("复制代码"), wxDefaultPosition, wxSize(80, -1));
    wxButton* sendBtn = new wxButton(panel, wxID_ANY, wxString::FromUTF8("发送"), wxDefaultPosition, wxSize(80, -1));
    sendBtn->SetDefault(); // 设置为默认按钮（回车触发）
    // 取消按钮（用于中断正在进行的请求）
    wxButton* cancelBtn = new wxButton(panel, wxID_ANY, wxString::FromUTF8("取消"), wxDefaultPosition, wxSize(80, -1));
    cancelBtn->Disable();

    // 3. 布局管理 (使用 Sizer)
    wxBoxSizer* outerSizer = new wxBoxSizer(wxHORIZONTAL);

    // 左侧：会话列表与操作按钮
    wxPanel* leftPanel = new wxPanel(panel, wxID_ANY);
    wxBoxSizer* leftSizer = new wxBoxSizer(wxVERTICAL);
    wxListBox* convoList = new wxListBox(leftPanel, wxID_ANY);
    wxButton* loadBtn = new wxButton(leftPanel, wxID_ANY, wxString::FromUTF8("加载"));
    wxButton* renameBtn = new wxButton(leftPanel, wxID_ANY, wxString::FromUTF8("重命名"));
    wxButton* exportBtn = new wxButton(leftPanel, wxID_ANY, wxString::FromUTF8("导出"));
    wxButton* deleteBtn = new wxButton(leftPanel, wxID_ANY, wxString::FromUTF8("删除"));

    // 填充已保存会话
    for (const auto& n : m_savedConversations) convoList->Append(wxString::FromUTF8(n));

    leftSizer->Add(convoList, 1, wxEXPAND | wxALL, 6);
    leftSizer->Add(loadBtn, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 6);
    leftSizer->Add(renameBtn, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 6);
    leftSizer->Add(exportBtn, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 6);
    leftSizer->Add(deleteBtn, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 6);
    leftPanel->SetSizer(leftSizer);

    // 右侧：历史对话与输入
    wxBoxSizer* rightSizer = new wxBoxSizer(wxVERTICAL);
    rightSizer->Add(historyCtrl, 1, wxEXPAND | wxLEFT | wxRIGHT, 10);
    rightSizer->Add(new wxStaticLine(panel), 0, wxEXPAND | wxALL, 10);

    wxBoxSizer* inputSizer = new wxBoxSizer(wxHORIZONTAL);
    inputSizer->Add(inputCtrl, 1, wxEXPAND | wxRIGHT, 10);
    inputSizer->Add(copyBtn, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5); // 复制按钮
    // 新对话按钮放在右侧输入区
    wxButton* newConvBtn = new wxButton(panel, wxID_ANY, wxString::FromUTF8("新对话"), wxDefaultPosition, wxSize(80, -1));
    inputSizer->Add(newConvBtn, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5); // 新对话按钮
    inputSizer->Add(cancelBtn, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);  // 取消按钮
    inputSizer->Add(sendBtn, 0, wxALIGN_CENTER_VERTICAL);               // 发送按钮

    rightSizer->Add(inputSizer, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);

    outerSizer->Add(leftPanel, 0, wxEXPAND | wxALL, 4);
    outerSizer->Add(rightSizer, 1, wxEXPAND | wxALL, 2);

    panel->SetSizer(outerSizer);

    // 4. 事件绑定

    // 新对话按钮：保存当前对话到本地历史并清空界面
    newConvBtn->Bind(wxEVT_BUTTON, [this, historyCtrl, convoList](wxCommandEvent&) {
        // 优先保存当前会话内存上下文，否则回退到 UI 文本
        std::string cur = this->m_currentSessionHistory.empty() ? std::string(historyCtrl->GetValue().ToUTF8().data()) : this->m_currentSessionHistory;
        // 生成基于时间戳的名称
        auto now = std::chrono::system_clock::now();
        std::time_t t = std::chrono::system_clock::to_time_t(now);
        std::tm tm;
        localtime_s(&tm, &t);
        std::ostringstream ss;
        ss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
        std::string name = std::string("会话 ") + ss.str();

        // 保存（允许空内容以表示一个新的空会话）
        this->AddConversation(name, cur);
        convoList->Append(wxString::FromUTF8(name));

        // 清空 UI 与会话状态，开启新对话
        historyCtrl->Clear();
        this->m_latestCode.Clear();
        this->memory_queue.Clear();
        this->memory.Clear();
        this->m_currentSessionName.clear();
        this->m_currentSessionHistory.clear();
        wxLogStatus(wxString::FromUTF8("对话已保存，本地新对话已创建。"));
    });
    
    // 加载会话到当前工作区（可以继续对话）
    loadBtn->Bind(wxEVT_BUTTON, [this, convoList, historyCtrl](wxCommandEvent&) {
        int sel = convoList->GetSelection();
        if (sel == wxNOT_FOUND) {
            wxLogStatus(wxString::FromUTF8("请先选择要加载的会话。"));
            return;
        }
        wxString name = convoList->GetString(sel);
        if (this->LoadConversationIntoSession(std::string(name.ToUTF8().data()))) {
            historyCtrl->SetValue(wxString::FromUTF8(this->m_currentSessionHistory));
            wxLogStatus(wxString::FromUTF8("会话已加载，可继续对话。"));
        }
    });

    // 重命名会话
    renameBtn->Bind(wxEVT_BUTTON, [this, convoList](wxCommandEvent&) {
        int sel = convoList->GetSelection();
        if (sel == wxNOT_FOUND) return;
        wxString oldName = convoList->GetString(sel);
        wxString newName = wxGetTextFromUser(wxString::FromUTF8("输入新的会话名称:"), wxString::FromUTF8("重命名会话"), oldName);
        if (newName.IsEmpty() || newName == oldName) return;
        this->RenameConversation(std::string(oldName.ToUTF8().data()), std::string(newName.ToUTF8().data()));
        convoList->SetString(sel, newName);
    });

    // 导出到文件
    exportBtn->Bind(wxEVT_BUTTON, [this, convoList](wxCommandEvent&) {
        int sel = convoList->GetSelection();
        if (sel == wxNOT_FOUND) return;
        wxString name = convoList->GetString(sel);
        wxFileDialog saveFile(nullptr, wxString::FromUTF8("导出会话到文件"), wxEmptyString, name + ".txt", wxString::FromUTF8("文本文件 (*.txt)|*.txt"), wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
        if (saveFile.ShowModal() == wxID_OK) {
            wxString path = saveFile.GetPath();
            this->ExportConversation(std::string(name.ToUTF8().data()), std::string(path.ToUTF8().data()));
            wxLogStatus(wxString::FromUTF8("会话已导出。"));
        }
    });

    // 删除会话
    deleteBtn->Bind(wxEVT_BUTTON, [this, convoList](wxCommandEvent&) {
        int sel = convoList->GetSelection();
        if (sel == wxNOT_FOUND) return;
        wxString name = convoList->GetString(sel);
        this->RemoveConversation(std::string(name.ToUTF8().data()));
        convoList->Delete(sel);
        wxLogStatus(wxString::FromUTF8("会话已删除。"));
    });
    

    // --- 修改区：复制按钮的点击事件 ---
    copyBtn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (this->m_latestCode.IsEmpty()) return;

        // 打开剪贴板并写入代码
        if (wxTheClipboard->Open()) {
            wxTheClipboard->SetData(new wxTextDataObject(this->m_latestCode));
            wxTheClipboard->Close();
            // 可以在主程序状态栏显示一条提示
            wxLogStatus(wxString::FromUTF8("代码已成功复制到剪贴板！"));
        }
        });

    // 取消按钮绑定：请求取消当前正在进行的网络请求
    cancelBtn->Bind(wxEVT_BUTTON, [this, panel, sendBtn, inputCtrl, cancelBtn, historyCtrl](wxCommandEvent&) {
        if (!this->m_requestInProgress) return;
        this->CancelCurrentRequest();
        wxLogStatus(wxString::FromUTF8("请求取消中..."));
        // 立即禁用取消按钮，等待回调恢复 UI
        cancelBtn->Disable();
    });

    // 5. 处理返回的事件（支持流分块、完成和取消信号）
    panel->Bind(EVT_AI_RESPONSE, [this, historyCtrl, sendBtn, inputCtrl, cancelBtn](wxThreadEvent& evt) {
        int code = evt.GetInt();
        // code meanings: 0 (default) = final non-stream content (string contains final AI-formatted reply)
        // 1 = partial stream chunk (append directly)
        // 2 = final stream content (string contains full AI-formatted reply)
        // 3 = cancelled
        // 4 = request finished (re-enable UI)

        if (code == 1) {
            // 部分流式数据，直接追加到历史窗口
            historyCtrl->AppendText(evt.GetString());
            historyCtrl->ShowPosition(historyCtrl->GetLastPosition());
            return;
        }

        if (code == 3) {
            wxMessageBox(evt.GetString(), wxString::FromUTF8("请求已取消"), wxOK | wxICON_INFORMATION);
            // re-enable UI
            if (sendBtn) sendBtn->Enable();
            if (inputCtrl) inputCtrl->Enable();
            if (cancelBtn) cancelBtn->Disable();
            return;
        }

        if (code == 4) {
            // 仅表示请求生命周期结束，恢复 UI
            if (sendBtn) sendBtn->Enable();
            if (inputCtrl) inputCtrl->Enable();
            if (cancelBtn) cancelBtn->Disable();
            return;
        }

        // 默认或 final (code == 0 or 2)
        wxString response = evt.GetString();

        // 1. 解析 AI 的严格格式回复
        DSResult res = ParseDSResponse(response);

        // 兜底机制：如果 AI 偶尔抽风没按格式返回，就直接显示原文
        if (res.analysis.IsEmpty() && res.code.IsEmpty()) {
            historyCtrl->AppendText(response + "\n");
            historyCtrl->ShowPosition(historyCtrl->GetLastPosition());
            // 追加完整原文到当前会话上下文
            try { this->m_currentSessionHistory += std::string(response.ToUTF8().data()) + "\n"; } catch (...) {}
            // 恢复 UI
            if (sendBtn) sendBtn->Enable();
            if (inputCtrl) inputCtrl->Enable();
            if (cancelBtn) cancelBtn->Disable();
            return;
        }

        // 2. 漂亮地分块显示（分析/代码/总结）
        historyCtrl->SetDefaultStyle(wxTextAttr(*wxBLUE));
        historyCtrl->AppendText(wxString::FromUTF8("\n[分析]\n"));
        historyCtrl->SetDefaultStyle(wxTextAttr(*wxBLACK));
        historyCtrl->AppendText(res.analysis + "\n");

        if (!res.code.IsEmpty()) {
            this->m_latestCode = res.code;
            historyCtrl->SetDefaultStyle(wxTextAttr(*wxBLUE));
            historyCtrl->AppendText(wxString::FromUTF8("\n[纯代码]\n"));
            historyCtrl->SetDefaultStyle(wxTextAttr(*wxBLACK));
            historyCtrl->AppendText(res.code + "\n");
        }

        // 如果处于自动创建文件模式，则在获取到代码后尝试将其写入到 projectRoot/src/<name>.v
        if (this->m_autoCreatePending && !res.code.IsEmpty()) {
            // Helper: extract multiple files from the provided code text
            auto ExtractFiles = [](const std::string& code) {
                std::map<std::string, std::string> files;

                // 1) 按 ==== filename ==== 格式分割
                size_t pos = 0;
                std::string marker = "==== ";
                bool foundMarker = false;
                while (true) {
                    size_t hdr = code.find(marker, pos);
                    if (hdr == std::string::npos) break;
                    foundMarker = true;
                    size_t nameStart = hdr + marker.size();
                    size_t nameEnd = code.find(" ====", nameStart);
                    if (nameEnd == std::string::npos) break;
                    std::string name = code.substr(nameStart, nameEnd - nameStart);
                    size_t contentStart = nameEnd + 5; // length of " ===="
                    // skip possible newline
                    if (contentStart < code.size() && (code[contentStart] == '\r' || code[contentStart] == '\n')) ++contentStart;
                    if (contentStart < code.size() && code[contentStart] == '\n') ++contentStart;
                    size_t next = code.find(marker, contentStart);
                    std::string content;
                    if (next == std::string::npos) content = code.substr(contentStart);
                    else content = code.substr(contentStart, next - contentStart);
                    // trim trailing whitespace
                    while (!content.empty() && (content.back()=='\n' || content.back()=='\r')) content.pop_back();
                    files[name] = content;
                    pos = next;
                    if (pos == std::string::npos) break;
                }

                if (!files.empty()) return files;

                // 2) 查找 ``` fenced code blocks，尝试从 fence 行中解析文件名，例如 ```verilog filename.v
                pos = 0;
                while (true) {
                    size_t f1 = code.find("```", pos);
                    if (f1 == std::string::npos) break;
                    size_t lineEnd = code.find('\n', f1);
                    if (lineEnd == std::string::npos) break;
                    std::string fenceLine = code.substr(f1+3, lineEnd - (f1+3));
                    // try to find a filename in the fenceLine
                    std::string filename;
                    // split by space and take last token if contains a dot
                    std::istringstream iss(fenceLine);
                    std::string token;
                    while (iss >> token) {
                        if (token.find('.') != std::string::npos) filename = token;
                    }
                    size_t f2 = code.find("```", lineEnd+1);
                    if (f2 == std::string::npos) break;
                    std::string content = code.substr(lineEnd+1, f2 - (lineEnd+1));
                    if (!filename.empty()) files[filename] = content;
                    pos = f2 + 3;
                }
                if (!files.empty()) return files;

                // 3) 尝试按 module ... endmodule 切分为多个文件
                std::string lower = code;
                for (auto &c : lower) c = (char)tolower(c);
                size_t searchPos = 0;
                std::vector<std::pair<size_t,size_t>> modRanges;
                while (true) {
                    size_t mpos = lower.find("module ", searchPos);
                    if (mpos == std::string::npos) break;
                    // find corresponding endmodule after mpos
                    size_t epos = lower.find("endmodule", mpos);
                    if (epos == std::string::npos) break;
                    // include the word 'endmodule' length
                    size_t endpos = epos + strlen("endmodule");
                    modRanges.emplace_back(mpos, endpos);
                    searchPos = endpos;
                }
                if (modRanges.size() > 1) {
                    // take any header before first module as common header
                    size_t headerEnd = modRanges.front().first;
                    std::string header = code.substr(0, headerEnd);
                    for (size_t i = 0; i < modRanges.size(); ++i) {
                        size_t s = modRanges[i].first;
                        size_t e = modRanges[i].second;
                        std::string block = code.substr(s, e - s);
                        // extract module name
                        size_t nameStart = s + strlen("module ");
                        while (nameStart < code.size() && isspace((unsigned char)code[nameStart])) ++nameStart;
                        size_t nameEnd = nameStart;
                        while (nameEnd < code.size() && (isalnum((unsigned char)code[nameEnd]) || code[nameEnd]=='_' || code[nameEnd]=='$')) ++nameEnd;
                        std::string modname = "module_" + std::to_string(i+1);
                        if (nameEnd > nameStart) modname = code.substr(nameStart, nameEnd - nameStart);
                        std::string fname = MakeSafeFilename(modname + ".v", ".v");
                        // include header in first file
                        if (i == 0 && !header.empty()) files[fname] = header + "\n" + block;
                        else files[fname] = block;
                    }
                    return files;
                }

                // 4) fallback: single unnamed file
                files["" ] = code;
                return files;
            };

            try {
                std::string codeUtf8 = std::string(res.code.ToUTF8().data());
                auto files = ExtractFiles(codeUtf8);

                namespace fs = std::filesystem;
                fs::path baseSrc = (!this->m_projectRoot.empty()) ? fs::path(this->m_projectRoot) : fs::current_path();
                fs::path srcDir = baseSrc / "src";
                fs::path libDir = baseSrc / "lib";
                std::error_code ec;
                fs::create_directories(srcDir, ec);
                fs::create_directories(libDir, ec);

                // If only one unnamed file, determine filename similarly to previous behavior
                if (files.size() == 1 && files.begin()->first.empty()) {
                    std::string filename = this->m_autoFilename;
                    if (filename.empty()) {
                        // try extract module name
                        std::string lower = codeUtf8;
                        for (auto &c : lower) c = (char)tolower(c);
                        size_t mpos = lower.find("module ");
                        if (mpos != std::string::npos) {
                            size_t nameStart = mpos + 7;
                            while (nameStart < codeUtf8.size() && isspace((unsigned char)codeUtf8[nameStart])) ++nameStart;
                            size_t nameEnd = nameStart;
                            while (nameEnd < codeUtf8.size() && (isalnum((unsigned char)codeUtf8[nameEnd]) || codeUtf8[nameEnd]=='_' || codeUtf8[nameEnd]=='$')) ++nameEnd;
                            if (nameEnd > nameStart) filename = codeUtf8.substr(nameStart, nameEnd - nameStart);
                        }
                    }
                    if (filename.empty()) {
                        auto now = std::chrono::system_clock::now();
                        std::time_t t = std::chrono::system_clock::to_time_t(now);
                        std::tm tm;
                        localtime_s(&tm, &t);
                        std::ostringstream ss;
                        ss << "auto_v_" << std::put_time(&tm, "%Y%m%d%H%M%S");
                        filename = ss.str();
                    }
                    filename = MakeSafeFilename(filename, ".v");
                    fs::path outPath = srcDir / filename;
                    std::ofstream ofs(outPath, std::ios::out | std::ios::binary);
                    if (ofs) {
                        ofs << files.begin()->second;
                        ofs.close();
                        wxString msg = wxString::FromUTF8("已在: ") + wxString::FromUTF8(outPath.string()) + wxString::FromUTF8(" 创建 Verilog 文件。");
                        wxLogStatus(msg);
                        historyCtrl->AppendText(msg + wxString::FromUTF8("\n"));
                    } else {
                        wxString msg = wxString::FromUTF8("错误：无法写入文件: ") + wxString::FromUTF8(outPath.string());
                        wxLogError(msg);
                        historyCtrl->AppendText(msg + wxString::FromUTF8("\n"));
                    }
                } else {
                    // multiple files (or named single file)
                    for (const auto &p : files) {
                        std::string fname = p.first;
                        std::string content = p.second;
                        fs::path outPath;
                        if (fname.empty()) {
                            // should not happen here, skip
                            continue;
                        }
                        // If filename contains directory path, preserve it relative to project root
                        fs::path fp(fname);
                        std::string base = fp.filename().string();
                        std::string ext = fp.extension().string();
                        if (ext.empty()) ext = ".v";
                        std::string safeBase = MakeSafeFilename(base, ext);
                        if (fp.has_parent_path()) {
                            fs::path parent = fp.parent_path();
                            // decide whether parent contains lib
                            if (parent.string().find("lib") != std::string::npos) {
                                fs::create_directories(libDir / parent, ec);
                                outPath = libDir / parent / safeBase;
                            } else {
                                fs::create_directories(srcDir / parent, ec);
                                outPath = srcDir / parent / safeBase;
                            }
                        } else {
                            // decide by extension/heuristic: header extensions go to lib
                            std::string lcExt = ext;
                            for (auto &c : lcExt) c = (char)tolower(c);
                            if (lcExt == ".svh" || lcExt == ".vh") {
                                outPath = libDir / safeBase;
                            } else {
                                outPath = srcDir / safeBase;
                            }
                        }

                        std::ofstream ofs(outPath, std::ios::out | std::ios::binary);
                        if (ofs) {
                            ofs << content;
                            ofs.close();
                            wxString msg = wxString::FromUTF8("已在: ") + wxString::FromUTF8(outPath.string()) + wxString::FromUTF8(" 创建文件。");
                            wxLogStatus(msg);
                            historyCtrl->AppendText(msg + wxString::FromUTF8("\n"));
                        } else {
                            wxString msg = wxString::FromUTF8("错误：无法写入文件: ") + wxString::FromUTF8(outPath.string());
                            wxLogError(msg);
                            historyCtrl->AppendText(msg + wxString::FromUTF8("\n"));
                        }
                    }
                }
            }
            catch (...) {
                wxLogError(wxString::FromUTF8("自动创建文件时发生异常。"));
            }

            // Reset flags
            this->m_autoCreatePending = false;
            this->m_autoFilename.clear();
            this->m_autoAllowMulti = false;
        }

        historyCtrl->SetDefaultStyle(wxTextAttr(wxColour(0, 128, 0)));
        historyCtrl->AppendText(wxString::FromUTF8("\n[总结]: "));
        historyCtrl->SetDefaultStyle(wxTextAttr(*wxBLACK));
        historyCtrl->AppendText(res.summary + "\n\n");

        if (!res.memory.IsEmpty()) {
            this->memory_queue.Add(res.memory);
            while (this->memory_queue.GetCount() > 10) {
                this->memory_queue.RemoveAt(0);
            }
            this->memory = wxString::FromUTF8("【之前的记忆上下文】:\n");
            for (const auto& m : this->memory_queue) this->memory += "- " + m + "\n";
            this->memory += wxString::FromUTF8("【记忆上下文结束】\n\n");
        }

        historyCtrl->ShowPosition(historyCtrl->GetLastPosition());

        try { this->m_currentSessionHistory += std::string(response.ToUTF8().data()) + "\n"; } catch (...) {}

        // 恢复 UI
        if (sendBtn) sendBtn->Enable();
        if (inputCtrl) inputCtrl->Enable();
        if (cancelBtn) cancelBtn->Disable();
    });

    auto onSend = [this, historyCtrl, inputCtrl, panel, sendBtn, cancelBtn](wxCommandEvent& event) {
        wxString userMsg = inputCtrl->GetValue();
        if (userMsg.IsEmpty()) return;

        // UI 反馈并禁用重复点击
        historyCtrl->SetDefaultStyle(wxTextAttr(*wxBLUE));
        historyCtrl->AppendText(wxString::FromUTF8("\n用户: ") + userMsg + "\n");
        historyCtrl->SetDefaultStyle(wxTextAttr(*wxBLACK));
        historyCtrl->AppendText(wxString::FromUTF8("DeepSeek: 正在思考...\n"));
        inputCtrl->Clear();
        if (sendBtn) sendBtn->Disable();
        if (inputCtrl) inputCtrl->Disable();
        if (cancelBtn) cancelBtn->Enable();

        std::string promptUtf8 = userMsg.ToUTF8().data();

        m_threads.emplace_back([this, panel, promptUtf8]() {
            // 调用 ProcessCommand（内部会在流式模式下向 panel 回传部分/最终事件）
            this->ProcessCommand(promptUtf8);

            if (m_isReleased) return;

            // 通知 UI 恢复
            wxThreadEvent* doneEvt = new wxThreadEvent(EVT_AI_RESPONSE);
            doneEvt->SetInt(4);
            wxQueueEvent(panel, doneEvt);
        });
    };

    sendBtn->Bind(wxEVT_BUTTON, [this, historyCtrl, inputCtrl, panel, sendBtn, cancelBtn](wxCommandEvent& event) {
        wxString userMsg = inputCtrl->GetValue();
        if (userMsg.IsEmpty()) return;
        // 将用户输入追加到当前会话上下文，以便后续消息带上历史
        this->m_currentSessionHistory += std::string("用户: ") + std::string(userMsg.ToUTF8().data()) + "\n";

        historyCtrl->SetDefaultStyle(wxTextAttr(*wxBLUE));
        historyCtrl->AppendText(wxString::FromUTF8("\n用户: ") + userMsg + "\n");
        historyCtrl->SetDefaultStyle(wxTextAttr(*wxBLACK));
        historyCtrl->AppendText(wxString::FromUTF8("DeepSeek: 正在思考...\n"));
        inputCtrl->Clear();
        if (sendBtn) sendBtn->Disable();
        if (inputCtrl) inputCtrl->Disable();
        if (cancelBtn) cancelBtn->Enable();

        std::string promptUtf8 = userMsg.ToUTF8().data();
        m_threads.emplace_back([this, panel, promptUtf8]() {
            this->ProcessCommand(promptUtf8);
            if (m_isReleased) return;
            wxThreadEvent* doneEvt = new wxThreadEvent(EVT_AI_RESPONSE);
            doneEvt->SetInt(4);
            wxQueueEvent(panel, doneEvt);
        });
    }, wxID_ANY);

    inputCtrl->Bind(wxEVT_TEXT_ENTER, [this, historyCtrl, inputCtrl, panel, sendBtn, cancelBtn](wxCommandEvent& event) {
        wxString userMsg = inputCtrl->GetValue();
        if (userMsg.IsEmpty()) return;

        historyCtrl->SetDefaultStyle(wxTextAttr(*wxBLUE));
        historyCtrl->AppendText(wxString::FromUTF8("\n用户: ") + userMsg + "\n");
        historyCtrl->SetDefaultStyle(wxTextAttr(*wxBLACK));
        historyCtrl->AppendText(wxString::FromUTF8("DeepSeek: 正在思考...\n"));
        inputCtrl->Clear();
        if (sendBtn) sendBtn->Disable();
        if (inputCtrl) inputCtrl->Disable();
        if (cancelBtn) cancelBtn->Enable();

        std::string promptUtf8 = userMsg.ToUTF8().data();
        m_threads.emplace_back([this, panel, promptUtf8]() {
            this->ProcessCommand(promptUtf8);
            if (m_isReleased) return;
            wxThreadEvent* doneEvt = new wxThreadEvent(EVT_AI_RESPONSE);
            doneEvt->SetInt(4);
            wxQueueEvent(panel, doneEvt);
        });
    }, wxID_ANY);

    return panel;
}


DSResult ParseDSResponse(const wxString& raw) {
    DSResult res;

    // 必须用 wxString::FromUTF8 告诉 wxWidgets 这些是 UTF-8 字符串
    wxString tag1 = wxString::FromUTF8("## 1. 分析");
    wxString tag2 = wxString::FromUTF8("## 2. 纯代码");
    wxString tag3 = wxString::FromUTF8("## 3. 简要总结");
    wxString tag4 = wxString::FromUTF8("## 4. 记忆存储");

    // 寻找位置
    int pos1 = raw.Find(tag1);
    int pos2 = raw.Find(tag2);
    int pos3 = raw.Find(tag3);
    int pos4 = raw.Find(tag4);

    // 1. 提取分析 (tag1 到 tag2 之间)
    if (pos1 != wxNOT_FOUND && pos2 != wxNOT_FOUND) {
        int start = pos1 + tag1.Length();
        res.analysis = raw.Mid(start, pos2 - start).Trim(true).Trim(false);
    }

    // 2. 提取代码 (tag2 到 tag3 之间)
    if (pos2 != wxNOT_FOUND && pos3 != wxNOT_FOUND) {
        int start = pos2 + tag2.Length();
        wxString rawCodePart = raw.Mid(start, pos3 - start).Trim(true).Trim(false);

        // 终极清洗法：直接暴力替换掉所有的 Markdown 代码块标记
        rawCodePart.Replace("```verilog", "");
        rawCodePart.Replace("```", "");

        res.code = rawCodePart.Trim(true).Trim(false);
    }

    // 3. 提取总结 (tag3 到 tag4 之间)
    if (pos3 != wxNOT_FOUND && pos4 != wxNOT_FOUND) {
        int start = pos3 + tag3.Length();
        res.summary = raw.Mid(start, pos4 - start).Trim(true).Trim(false);
    }

    // 4. 提取记忆 (tag4 到末尾)
    if (pos4 != wxNOT_FOUND) {
        int start = pos4 + tag4.Length();
        res.memory = raw.Mid(start).Trim(true).Trim(false);
    }

    return res;
}
