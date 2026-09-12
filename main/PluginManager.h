#pragma once
#include "ISigPlugin.h"
#include <vector>
#include <string>
#include <windows.h>
#include <filesystem>
#include <map>

class PluginManager {
public:
    PluginManager() = default;
    ~PluginManager();

    // 扫描并加载指定目录下的所有 DLL
    void LoadPlugins(const std::string& folderPath);

    // 根据名称获取插件
    ISigPlugin* GetPlugin(const std::string& name);

    // 获取所有已加载插件的列表
    const std::vector<ISigPlugin*>& GetAllPlugins() const { return m_plugins; }

    // 卸载所有插件并清理内存
    void UnloadAll();

private:
    struct PluginInfo {
        HMODULE hModule;
        ISigPlugin* pInstance;
    };

    std::vector<ISigPlugin*> m_plugins;
    std::vector<PluginInfo> m_loadedModules; // 维护句柄和实例的对应关系
};
