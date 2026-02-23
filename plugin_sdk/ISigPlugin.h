#pragma once
#include <string>
#include <wx/wx.h>

// 插件接口定义
class ISigPlugin {
public:
    virtual ~ISigPlugin() {}

    // 插件基本信息
    virtual std::string GetName() const = 0;
    
    // 核心转换功能：将你的命令语言转为 Verilog
    virtual wxPanel* CreatePanel(wxWindow* parent) = 0;
    virtual std::string ProcessCommand(const std::string& cmd) = 0;

    // 释放内存的“自毁”函数（防止跨 DLL 删除导致的崩溃）
    virtual void Release() = 0;

    // 可选：宿主程序可以告知插件当前打开的项目/工作区根路径
    // 插件可以重写此方法以接收项目根路径；默认实现为空，保持向后兼容
    virtual void SetProjectRoot(const std::string& /*path*/) { }
};

// 导出函数的原型定义，方便内核 GetProcAddress
typedef ISigPlugin* (__cdecl* CreatePluginFunc)();
