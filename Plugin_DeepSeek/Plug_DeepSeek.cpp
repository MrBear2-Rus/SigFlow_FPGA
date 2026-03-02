#include "pch.h"
#include "Plug_DeepSeek.h"

#include <wx/statline.h>
#include <iostream>
#include <winhttp.h>
#include <nlohmann/json.hpp> // 需要安装 json 库
#include <Windows.h>

#pragma comment(lib, "winhttp.lib") // 告诉编译器自动链接 winhttp 库
using json = nlohmann::json;
struct DSResult {
    wxString analysis;  // 1. 逻辑分析过程
    wxString code;      // 2. 提取出的纯 Verilog 代码
    wxString summary;   // 3. 改动简要总结
    wxString memory;    // 4. 待存储的长期记忆点
};

DSResult ParseDSResponse(const wxString& raw);








wxDEFINE_EVENT(EVT_AI_RESPONSE, wxThreadEvent);

Plug_DeepSeek::Plug_DeepSeek() {
    m_apiKey = "sk-8801be45326a4776ac37f3b120ee1888"; // 实际开发建议从配置文件读取
    m_apiUrl = "https://api.deepseek.com/chat/completions";

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
    // 这里构造发送给 DS 的 Prompt
    std::string fullPrompt = memory.ToStdString() +

        "你是一个严格遵守格式的 Verilog 专家。无论用户问什么，你都必须且只能按以下格式回复，严禁任何前言和后语：\n"
    "## 1. 分析\n...\n"
    "## 2. 纯代码\n...\n"
    "## 3. 简要总结\n...\n"
    "## 4. 记忆存储\n...\n\n"
    "现在开始！用户的请求是：" + cmd;

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
    inputCtrl->SetHint("输入问题，按回车或点击发送...");

    // 发送按钮
    wxButton* sendBtn = new wxButton(panel, wxID_ANY, "发送", wxDefaultPosition, wxSize(100, -1));
    sendBtn->SetDefault(); // 设置为默认按钮（回车触发）

    // 3. 布局管理 (使用 Sizer)
    wxBoxSizer* mainSizer = new wxBoxSizer(wxVERTICAL);
    wxBoxSizer* inputSizer = new wxBoxSizer(wxHORIZONTAL);

    // 将历史框放入主布局 (比例为 1，填满剩余空间)
    mainSizer->Add(historyCtrl, 1, wxEXPAND | wxLEFT | wxRIGHT, 10);
    mainSizer->Add(new wxStaticLine(panel), 0, wxEXPAND | wxALL, 10);

    // 将输入框和按钮放入横向布局
    inputSizer->Add(inputCtrl, 1, wxEXPAND | wxRIGHT, 10);
    inputSizer->Add(sendBtn, 0, wxALIGN_CENTER_VERTICAL);

    mainSizer->Add(inputSizer, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);

    panel->SetSizer(mainSizer);

    // 4. 事件绑定
    auto onSend = [this, historyCtrl, inputCtrl, panel](wxCommandEvent& event) {
        wxString userMsg = inputCtrl->GetValue();
        if (userMsg.IsEmpty()) return;

        // UI 反馈
        historyCtrl->SetDefaultStyle(wxTextAttr(*wxBLUE));
        historyCtrl->AppendText("\n用户: " + userMsg + "\n");
        historyCtrl->SetDefaultStyle(wxTextAttr(*wxBLACK));
        historyCtrl->AppendText("DeepSeek: 正在思考...\n");
        inputCtrl->Clear();

        std::string promptUtf8 = userMsg.ToUTF8().data();

        m_threads.emplace_back([this, panel, promptUtf8]() { // 确保按值捕获 promptUtf8
            // 这里的 promptUtf8 现在是安全的 UTF-8 字节流
            std::string response = this->CallDeepSeekAPI(promptUtf8);

            if (m_isReleased) return;

            wxThreadEvent* evt = new wxThreadEvent(EVT_AI_RESPONSE);
            // API 返回的通常也是 UTF-8，后面处理记得 FromUTF8
            evt->SetString(wxString::FromUTF8(response));
            wxQueueEvent(panel, evt);
            });
        };

    // 5. 处理返回的事件
    panel->Bind(EVT_AI_RESPONSE, [this, historyCtrl](wxThreadEvent& evt) {
        // 这里的代码会在【主线程/UI线程】执行，安全更新控件
        wxString response = evt.GetString();

        historyCtrl->AppendText(response + "\n");
        /*
        DSResult res =  ParseDSResponse(response);

        historyCtrl->SetDefaultStyle(wxTextAttr(*wxBLUE));
        historyCtrl->AppendText("\n[分析]\n");
        historyCtrl->SetDefaultStyle(wxTextAttr(*wxBLACK));
        historyCtrl->AppendText(res.analysis + "\n");

        historyCtrl->SetDefaultStyle(wxTextAttr(*wxBLUE));
        historyCtrl->AppendText("\n[代码]\n");
        historyCtrl->SetDefaultStyle(wxTextAttr(*wxBLACK));
        historyCtrl->AppendText(res.code + "\n");

        historyCtrl->SetDefaultStyle(wxTextAttr(wxColour(0, 128, 0))); // 暗绿色
        historyCtrl->AppendText("[总结]: ");
        historyCtrl->SetDefaultStyle(wxTextAttr(*wxBLACK));
        historyCtrl->AppendText(res.summary + "\n");

        if (!res.memory.empty()) {
            // 我们可以维护一个 wxArrayString 成员变量 m_memoryList
            memory_queue.Add(res.memory);

            // 2. 数量控制：比如只保留最近的 10 条最重要的记忆
            while (memory_queue.GetCount() > 10) {
                memory_queue.RemoveAt(0); // 删掉最老的
            }

            // 3. 重新合成给 AI 看的记忆字符串
            this->memory = "";
            for (const auto& m : memory_queue) {
                this->memory += "- " + m + "\n";
            }
        }*/

        historyCtrl->ShowPosition(historyCtrl->GetLastPosition());
        });

    sendBtn->Bind(wxEVT_BUTTON, onSend);
    inputCtrl->Bind(wxEVT_TEXT_ENTER, onSend);

    return panel;
}

DSResult ParseDSResponse(const wxString& raw) {
    DSResult res;

    // 定义四个锚点
    wxString tag1 = "## 1. 分析";
    wxString tag2 = "## 2. 纯代码";
    wxString tag3 = "## 3. 简要总结";
    wxString tag4 = "## 4. 记忆存储";

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

        // 清洗 Markdown 代码块标记 ```verilog ... ```
        if (rawCodePart.Contains("```")) {
            // 提取第一个 ``` 之后的内容，再去掉结尾的 ```
            wxString content = rawCodePart.AfterFirst('\n'); // 跳过 ```verilog 那一行
            if (content.Contains("```")) {
                res.code = content.BeforeLast('`').Trim(true).Trim(false);
                // 此时 res.code 里的反引号可能还没清干净，做个彻底清理
                res.code.Replace("```", "");
            }
            else {
                res.code = content.Trim(true).Trim(false);
            }
        }
        else {
            res.code = rawCodePart;
        }
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
