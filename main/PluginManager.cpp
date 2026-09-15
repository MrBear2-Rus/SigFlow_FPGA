#include "PluginManager.h"
#include "platform/PlatformPaths.h"
#include <iostream>

namespace fs = std::filesystem;

PluginManager::~PluginManager() {
    UnloadAll();
}

void PluginManager::LoadPlugins(const std::string& folderPath) {
    fs::path absolutePath = fs::absolute(folderPath);
    if (!fs::exists(absolutePath)) return;

    for (const auto& entry : fs::directory_iterator(absolutePath)) {
        if (entry.path().extension() == sigflow::platform::SharedLibrarySuffix()) {
            std::string dllPath = entry.path().string();

            // 1. 加载动态库模块
            auto library = std::make_unique<sigflow::platform::DynamicLibrary>();
            if (!library->Load(dllPath)) {
                continue;
            }

            // 2. 查找工厂函数 CreateSigPlugin
            // 注意：DLL 侧必须使用 extern "C" 导出
            CreatePluginFunc createFunc =
                reinterpret_cast<CreatePluginFunc>(library->Symbol("CreateSigPlugin"));

            if (createFunc) {
                // 3. 创建插件实例
                ISigPlugin* pPlugin = createFunc();
                if (pPlugin) {
                    m_plugins.push_back(pPlugin);
                    m_loadedModules.push_back({ std::move(library), pPlugin });
                }
                else {
                    library->Unload();
                }
            }
            else {
                // 没找到导出函数，释放句柄
                library->Unload();
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
    // 关键：先释放插件实例，再卸载动态库句柄
    // 如果先卸载动态库，释放实例时的虚函数代码就找不到了，会报异常
    for (auto& info : m_loadedModules) {
        if (info.instance) {
            info.instance->Release(); // 使用接口定义的 Release 自毁
        }
        if (info.library) {
            info.library->Unload();
        }
    }
    m_plugins.clear();
    m_loadedModules.clear();
}
