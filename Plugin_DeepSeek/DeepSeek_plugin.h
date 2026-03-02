#pragma once
#include "pch.h" // 必须放在第一行
#include ""

class MyDsPlugin : public ISigPlugin {
public:
    std::string GetName() const override { return "DeepSeek_Plugin"; }

    std::string ProcessCommand(const std::string& cmd) override {
        // 这里就是你未来写 DeepSeek API 调用的地方
        return "// Generated Verilog for: " + cmd;
    }

    void Release() override { delete this; }
};

// 关键！导出“工厂函数”，让内核能“抓”到这个插件
extern "C" __declspec(dllexport) ISigPlugin* CreateSigPlugin() {
    return new MyDsPlugin();
}