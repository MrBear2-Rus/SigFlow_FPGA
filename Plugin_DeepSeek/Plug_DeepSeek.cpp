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

#pragma comment(lib, "winhttp.lib") // 告诉编译器自动链接 winhttp 库
using json = nlohmann::json;
struct DSResult {
    wxString analysis;  // 1. 逻辑分析过程
    wxString code;      // 2. 提取出的纯 Verilog 代码
    wxString summary;   // 3. 改动简要总结
    wxString memory;    // 4. 待存储的长期记忆点
};

DSResult ParseDSResponse(const wxString& raw);

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
    std::string fullPrompt;
    const std::string scanCmd = "/scanproject";
    const std::string scanCmd2 = "/scan";

    if (cmd.rfind(scanCmd, 0) == 0 || cmd.rfind(scanCmd2, 0) == 0) {
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

    return CallDeepSeekAPI(fullPrompt);
}

std::string Plug_DeepSeek::CallDeepSeekAPI(const std::string& prompt) {
    std::string responseData;
    HINTERNET hSession = NULL, hConnect = NULL, hRequest = NULL;

    // 1. 初始化 WinHTTP (增加对 HTTPS 协议的兼容性支持)
    hSession = WinHttpOpen(L"EDA Assistant/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return "Error: WinHttpOpen failed.";
    else WinHttpSetTimeouts(hSession, 60000, 60000, 60000, 120000);

    // 2. 指定服务器
    hConnect = WinHttpConnect(hSession, L"api.deepseek.com", INTERNET_DEFAULT_HTTPS_PORT, 0);

    if (hConnect) {
        // 3. 创建请求 (确保开启了 WINHTTP_FLAG_SECURE 以支持 HTTPS)
        hRequest = WinHttpOpenRequest(hConnect, L"POST", L"/chat/completions", NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    }

    if (hRequest) {
        // 4. 构造 Payload 和 Header
        json payload = {
            {"model", "deepseek-chat"},
            {"messages", {{{"role", "user"}, {"content", prompt}}}},
            {"temperature", 0.0},
            {"stream", false}
        };
        std::string jsonStr = payload.dump();

        // 转换 API Key (注意：确保 m_apiKey 不为空)
        std::wstring wKey(m_apiKey.begin(), m_apiKey.end());
        std::wstring headers = L"Content-Type: application/json\r\nAuthorization: Bearer " + wKey + L"\r\n";

        // 5. 发送请求
        BOOL bResults = WinHttpSendRequest(hRequest, headers.c_str(), (DWORD)-1L, (LPVOID)jsonStr.c_str(), (DWORD)jsonStr.length(), (DWORD)jsonStr.length(), 0);

        // 6. 接收返回内容
        if (bResults) bResults = WinHttpReceiveResponse(hRequest, NULL);

        if (bResults) {
            DWORD dwSize = 0;
            do {
                if (!WinHttpQueryDataAvailable(hRequest, &dwSize)) break;
                if (dwSize == 0) break;

                char* pszOutBuffer = new char[dwSize + 1];
                DWORD dwDownloaded = 0;
                if (WinHttpReadData(hRequest, (LPVOID)pszOutBuffer, dwSize, &dwDownloaded)) {
                    responseData.append(pszOutBuffer, dwDownloaded);
                }
                delete[] pszOutBuffer;
            } while (dwSize > 0);
        }
    }

    // --- 资源清理 (提前执行，防止内存泄漏) ---
    if (hRequest) WinHttpCloseHandle(hRequest);
    if (hConnect) WinHttpCloseHandle(hConnect);
    if (hSession) WinHttpCloseHandle(hSession);

    if (responseData.empty()) return "Error: No data from API.";

    // --- 7. 安全解析 (核心修改区) ---
    try {
        auto resJson = json::parse(responseData);

        // A. 检查 DeepSeek 是否返回了业务错误 (如 API Key 失效、余额不足)
        if (resJson.contains("error")) {
            return "DeepSeek API Error: " + resJson["error"]["message"].get<std::string>();
        }

        // B. 严谨地层层验证 JSON 结构
        if (resJson.contains("choices") && resJson["choices"].is_array() && !resJson["choices"].empty()) {
            auto& firstChoice = resJson["choices"][0];
            if (firstChoice.contains("message") && firstChoice["message"].contains("content")) {
                return firstChoice["message"]["content"].get<std::string>();
            }
        }

        return "Error: Unexpected JSON format. Raw Response: " + responseData;
    }
    catch (const json::exception& e) {
        // 捕获 nlohmann::json 抛出的类型错误或解析错误
        return "JSON Error: " + std::string(e.what()) + "\nRaw data: " + responseData;
    }
    catch (...) {
        return "Critical Error: An unknown exception occurred during parsing.";
    }
}

// 在 Plug_DeepSeek.cpp 中
extern "C" __declspec(dllexport) ISigPlugin* CreateSigPlugin() {
    return new Plug_DeepSeek();
}

wxPanel* Plug_DeepSeek::CreatePanel(wxWindow* parent) {
    // 1. 创建主面板
    wxPanel* panel = new wxPanel(parent, wxID_ANY);
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

    // 3. 布局管理 (使用 Sizer)
    wxBoxSizer* mainSizer = new wxBoxSizer(wxVERTICAL);
    wxBoxSizer* inputSizer = new wxBoxSizer(wxHORIZONTAL);

    // 将历史框放入主布局 (比例为 1，填满剩余空间)
    mainSizer->Add(historyCtrl, 1, wxEXPAND | wxLEFT | wxRIGHT, 10);
    mainSizer->Add(new wxStaticLine(panel), 0, wxEXPAND | wxALL, 10);

    // --- 修改区：将复制按钮加入横向布局 ---
    inputSizer->Add(inputCtrl, 1, wxEXPAND | wxRIGHT, 10);
    inputSizer->Add(copyBtn, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5); // 复制按钮
    inputSizer->Add(sendBtn, 0, wxALIGN_CENTER_VERTICAL);              // 发送按钮

    mainSizer->Add(inputSizer, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);

    panel->SetSizer(mainSizer);

    // 4. 事件绑定
    auto onSend = [this, historyCtrl, inputCtrl, panel](wxCommandEvent& event) {
        wxString userMsg = inputCtrl->GetValue();
        if (userMsg.IsEmpty()) return;

        // UI 反馈
        historyCtrl->SetDefaultStyle(wxTextAttr(*wxBLUE));
        historyCtrl->AppendText(wxString::FromUTF8("\n用户: ") + userMsg + "\n");
        historyCtrl->SetDefaultStyle(wxTextAttr(*wxBLACK));
        historyCtrl->AppendText(wxString::FromUTF8("DeepSeek: 正在思考...\n"));
        inputCtrl->Clear();

        std::string promptUtf8 = userMsg.ToUTF8().data();

        m_threads.emplace_back([this, panel, promptUtf8]() {
            // 修复业务 Bug：改为调用 ProcessCommand，让用户输入穿上“提示词马甲”后再发给 API
            std::string response = this->ProcessCommand(promptUtf8);

            if (m_isReleased) return;

            wxThreadEvent* evt = new wxThreadEvent(EVT_AI_RESPONSE);
            // API 返回的纯正 UTF-8 解析为 wxString
            evt->SetString(wxString::FromUTF8(response));
            wxQueueEvent(panel, evt);
            });
        };

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

    // 5. 处理返回的事件
    panel->Bind(EVT_AI_RESPONSE, [this, historyCtrl](wxThreadEvent& evt) {
        // 这里的代码会在【主线程/UI线程】执行，安全更新控件
        wxString response = evt.GetString();

        // 1. 解析 AI 的严格格式回复
        DSResult res = ParseDSResponse(response);

        // 兜底机制：如果 AI 偶尔抽风没按格式返回，就直接显示原文
        if (res.analysis.IsEmpty() && res.code.IsEmpty()) {
            historyCtrl->AppendText(response + "\n");
            historyCtrl->ShowPosition(historyCtrl->GetLastPosition());
            return;
        }

        // 2. 漂亮地分块显示
        // [分析] - 蓝色标题，黑色内容
        historyCtrl->SetDefaultStyle(wxTextAttr(*wxBLUE));
        historyCtrl->AppendText(wxString::FromUTF8("\n[分析]\n"));
        historyCtrl->SetDefaultStyle(wxTextAttr(*wxBLACK));
        historyCtrl->AppendText(res.analysis + "\n");

        // [代码] - 蓝色标题，黑色内容
        if (!res.code.IsEmpty()) {
            // --- 修改区：将最新解析出来的代码存入变量，供复制按钮使用 ---
            this->m_latestCode = res.code;

            historyCtrl->SetDefaultStyle(wxTextAttr(*wxBLUE));
            historyCtrl->AppendText(wxString::FromUTF8("\n[纯代码]\n"));
            historyCtrl->SetDefaultStyle(wxTextAttr(*wxBLACK));
            historyCtrl->AppendText(res.code + "\n");
        }

        // [总结] - 暗绿色标题，同行黑色内容
        historyCtrl->SetDefaultStyle(wxTextAttr(wxColour(0, 128, 0)));
        historyCtrl->AppendText(wxString::FromUTF8("\n[总结]: "));
        historyCtrl->SetDefaultStyle(wxTextAttr(*wxBLACK));
        historyCtrl->AppendText(res.summary + "\n\n");

        // 3. 处理长期记忆 (对用户不可见，只在底层运转)
        if (!res.memory.IsEmpty()) {
            this->memory_queue.Add(res.memory);
            while (this->memory_queue.GetCount() > 10) {
                this->memory_queue.RemoveAt(0);
            }
            this->memory = wxString::FromUTF8("【之前的记忆上下文】:\n");
            for (const auto& m : this->memory_queue) {
                this->memory += "- " + m + "\n";
            }
            this->memory += wxString::FromUTF8("【记忆上下文结束】\n\n");
        }

        // 滚动到最底部
        historyCtrl->ShowPosition(historyCtrl->GetLastPosition());
        });

    sendBtn->Bind(wxEVT_BUTTON, onSend);
    inputCtrl->Bind(wxEVT_TEXT_ENTER, onSend);

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
