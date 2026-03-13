#pragma once
#include "pch.h"
#include "ISigPlugin.h" // 确保路径指向你的 SDK 目录
#include <string>
#include <wx/wx.h>
#include <thread>
#include <map>
#include <vector>
#include <atomic>
#include <mutex>
#include <winhttp.h>
#include <fstream>
#include <memory>

class Plug_DeepSeek : public ISigPlugin {
public:
    Plug_DeepSeek();
    virtual ~Plug_DeepSeek();

    // 实现接口方法
    std::string GetName() const override;
    wxPanel* CreatePanel(wxWindow* parent) override;
    std::string ProcessCommand(const std::string& cmd) override;
    void Release() override;
    void SetProjectRoot(const std::string& path) override;

private:
    std::atomic<int> m_aiPhase{ 0 }; // 0=idle, 1=design, 2=generate
    std::string m_pendingGenerationPrompt;
    std::mutex m_pendingPromptMutex;
    std::string m_apiKey;
    std::string m_apiUrl;

    wxString memory;
    wxString m_latestCode;
    wxArrayString memory_queue;

    // 持久化的历史对话数据（保存在本地）
    std::vector<std::string> m_savedConversations; // 列表（名称）
    std::map<std::string, std::string> m_conversationContents; // 名称 -> 内容
    std::string m_dataDir;     // 本地存储目录
    std::string m_historyFile; // 历史记录 JSON 文件路径
    // 当前会话状态（用于在加载/恢复会话后继续对话）
    std::string m_currentSessionName;
    std::string m_currentSessionHistory; // 仅文本回放/上下文
    bool m_currentSessionIsPlaceholder = false; // 标记当前会话名为占位符（例如“新对话”）
    void GenerateAndSetSessionTitle(const std::string& firstUserMsg, wxWindow* panel);
    std::atomic<bool> m_isReleased{false}; 
    std::vector<std::thread> m_threads;
    std::string m_projectRoot;
    // 当前 Panel 指针（用于流式分块回传 UI）
    wxWindow* m_panel = nullptr;

    // 自动生成文件模式：当用户使用 /genfile 命令时置位，
    // 在收到 AI 代码输出后插件会自动将代码写入到项目的 src 目录
    std::atomic<bool> m_autoCreatePending{false};
    std::string m_autoFilename; // 可选的用户指定文件名
    // 当使用 /autogen 时允许多文件输出解析
    std::atomic<bool> m_autoAllowMulti{false};

    // 请求管理与取消支持
    std::atomic<bool> m_requestInProgress{false};
    std::atomic<bool> m_cancelRequest{false};
    std::mutex m_requestMutex;
    // WinHTTP 句柄快照，用于取消
    HINTERNET m_hSessionHandle = NULL;
    HINTERNET m_hConnectHandle = NULL;
    HINTERNET m_hRequestHandle = NULL;

    // Generation-time temporary buffering & backups
    std::string m_generationTempPath; // path to temporary aggregated response during generation
    std::unique_ptr<std::ofstream> m_generationTempStream; // stream writing incremental response
    std::mutex m_generationMutex; // protects generation temp stream and backup structures
    std::map<std::string, std::string> m_generationBackups; // path -> original contents (for rollback)
    std::vector<std::string> m_generationCreatedFiles; // files created by generation (to remove on reject)
    bool m_generationActive = false; // whether a generation is in progress

    // 内部辅助函数：处理网络请求
    static size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp);
    // 如果 panel 非空且 stream 为 true，则会使用增量回传（流式）并在完成时返回完整响应字符串
    std::string CallDeepSeekAPI(const std::string& prompt, wxWindow* panel = nullptr, bool stream = false);
    // 取消正在进行的请求（线程安全）
    void CancelCurrentRequest();

    // 本地持久化相关
    void SaveConversationsToDisk();
    // AddConversation returns the actual stored name (may be suffixed to avoid collisions)
    std::string AddConversation(const std::string& name, const std::string& content);
    void RemoveConversation(const std::string& name);
    void RenameConversation(const std::string& oldName, const std::string& newName);
    void ExportConversation(const std::string& name, const std::string& path);
    bool LoadConversationIntoSession(const std::string& name);
};

// 导出工厂函数
extern "C" __declspec(dllexport) ISigPlugin* CreateSigPlugin();
