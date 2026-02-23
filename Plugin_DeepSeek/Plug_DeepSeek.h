#pragma once
#include "pch.h"
#include "ISigPlugin.h" // 确保路径指向你的 SDK 目录
#include <string>
#include <wx/wx.h>
#include <thread>

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

    std::atomic<bool> m_isReleased{false}; 
    std::vector<std::thread> m_threads;
    std::string m_projectRoot;

    // 内部辅助函数：处理网络请求
    static size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp);
    std::string CallDeepSeekAPI(const std::string& prompt);
};

// 导出工厂函数
extern "C" __declspec(dllexport) ISigPlugin* CreateSigPlugin();
