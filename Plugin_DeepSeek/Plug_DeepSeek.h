#pragma once
#include "pch.h"
#include "ISigPlugin.h" // 确保路径指向你的 SDK 目录
#include <string>
#include <wx/wx.h>
#include <thread>
#include <map>
#include <vector>

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

    std::atomic<bool> m_isReleased{false}; 
    std::vector<std::thread> m_threads;
    std::string m_projectRoot;

    // 内部辅助函数：处理网络请求
    static size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp);
    std::string CallDeepSeekAPI(const std::string& prompt);

    // 本地持久化相关
    void SaveConversationsToDisk();
    void AddConversation(const std::string& name, const std::string& content);
    void RemoveConversation(const std::string& name);
    void RenameConversation(const std::string& oldName, const std::string& newName);
    void ExportConversation(const std::string& name, const std::string& path);
    bool LoadConversationIntoSession(const std::string& name);
};

// 导出工厂函数
extern "C" __declspec(dllexport) ISigPlugin* CreateSigPlugin();
