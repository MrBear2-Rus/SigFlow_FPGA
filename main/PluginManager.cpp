#include "PluginManager.h"
#include <iostream>

namespace fs = std::filesystem;

PluginManager::~PluginManager() {
    UnloadAll();
}

void PluginManager::LoadPlugins(const std::string& folderPath) {
    fs::path absolutePath = fs::absolute(folderPath);
    if (!fs::exists(absolutePath)) return;

    for (const auto& entry : fs::directory_iterator(absolutePath)) {
        if (entry.path().extension() == ".dll") {
            std::string dllPath = entry.path().string();

            // 1. 加载 DLL 模块
            HMODULE hModule = LoadLibraryA(dllPath.c_str());
            if (!hModule) {
                // 建议使用 OutputDebugStringA 方便在 VS 调试窗口看错误
                continue;
            }

            // 2. 查找工厂函数 CreateSigPlugin
            // 注意：DLL 侧必须使用 extern "C" __declspec(dllexport)
            CreatePluginFunc createFunc = (CreatePluginFunc)GetProcAddress(hModule, "CreateSigPlugin");

            if (createFunc) {
                // 3. 创建插件实例
                ISigPlugin* pPlugin = createFunc();
                if (pPlugin) {
                    m_plugins.push_back(pPlugin);
                    m_loadedModules.push_back({ hModule, pPlugin });
                }
                else {
                    FreeLibrary(hModule);
                }
            }
            else {
                // 没找到导出函数，释放句柄
                FreeLibrary(hModule);
            }
        }
    }
}

ISigPlugin* PluginManager::GetPlugin(const std::string& name) {
    for (auto p : m_plugins) {
        if (p->GetName() == name) return p;
    }
    return nullptr;
}

void PluginManager::UnloadAll() {
    // 关键：先释放插件实例，再卸载 DLL 句柄
    // 如果先卸载 DLL，释放实例时的虚构函数代码就找不到了，会报异常
    for (auto& info : m_loadedModules) {
        if (info.pInstance) {
            info.pInstance->Release(); // 使用接口定义的 Release 自毁
        }
        if (info.hModule) {
            FreeLibrary(info.hModule);
        }
    }
    m_plugins.clear();
    m_loadedModules.clear();
}
