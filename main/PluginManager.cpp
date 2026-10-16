#include "PluginManager.h"
#include "platform/PlatformPaths.h"
#include "platform/Log.h"
#include <iostream>
#include <exception>

namespace fs = std::filesystem;

PluginManager::~PluginManager() {
    UnloadAll();
}

void PluginManager::LoadPlugins(const std::string& folderPath) {
    // folderPath 约定为 UTF-8（调用方 MainFrame 用 platform::Utf8String 转换）。
    // 旧实现直接 fs::path(std::string) / path::string()：Windows 上走 ANSI 代码页，
    // 非 ASCII 安装路径会被转成空串 → fs::absolute("") 变成**当前工作目录**，
    // 于是真正的 plugins 目录从不被扫描，反而去加载 CWD 里的任意 DLL。
    const fs::path absolutePath = fs::absolute(sigflow::platform::Utf8Path(folderPath));

    std::error_code existsEc;
    if (!fs::exists(absolutePath, existsEc)) {
        // 旧实现静默 return：插件目录放错位置时完全不可见。
        SIGFLOW_LOG("PluginManager: plugin directory not found: " + folderPath + "\n");
        return;
    }

    std::error_code iterateEc;
    for (fs::directory_iterator it(absolutePath, iterateEc), end; it != end;
         it.increment(iterateEc)) {
        if (iterateEc) {
            // 权限不足等错误以前会直接抛 filesystem_error，逃出 MainFrame 构造函数
            // 就是启动崩溃。这里改为记录并停止枚举。
            SIGFLOW_LOG("PluginManager: enumeration failed: " + iterateEc.message() + "\n");
            break;
        }
        const auto& entry = *it;
        if (sigflow::platform::ExtensionEquals(
                entry.path().extension().string(),
                sigflow::platform::SharedLibrarySuffix())) {
            // 必须转成 UTF-8 再交给 DynamicLibrary（见其 Load 的说明）。
            std::string dllPath = sigflow::platform::PathToUtf8(entry.path());

            // 1. 加载动态库模块
            auto library = std::make_unique<sigflow::platform::DynamicLibrary>();
            if (!library->Load(dllPath)) {
                // 旧实现静默 continue：插件加载失败时界面上完全看不出原因。
                SIGFLOW_LOG("PluginManager: failed to load " + dllPath +
                            " (" + library->LastError() + ")\n");
                continue;
            }

            // 2. 查找工厂函数 CreateSigPlugin
            // 注意：DLL 侧必须使用 extern "C" 导出
            CreatePluginFunc createFunc =
                reinterpret_cast<CreatePluginFunc>(library->Symbol("CreateSigPlugin"));

            if (createFunc) {
                // 3. 创建插件实例
                // 插件构造函数可能抛异常（例如在非 ASCII 用户目录下，插件内部用
                // std::filesystem::path(窄字符串) 做转换会抛 filesystem 转换错误）。
                // 异常一旦逃出 MainFrame 构造函数，表现就是启动即
                // "Unhandled unknown exception"。这里隔离：某个插件创建失败只记录
                // 日志并跳过它，不影响主程序启动与其它插件。
                ISigPlugin* pPlugin = nullptr;
                try {
                    pPlugin = createFunc();
                }
                catch (const std::exception& e) {
                    SIGFLOW_LOG("PluginManager: CreateSigPlugin threw for " + dllPath +
                                ": " + e.what() + "\n");
                }
                catch (...) {
                    SIGFLOW_LOG("PluginManager: CreateSigPlugin threw unknown exception for " +
                                dllPath + "\n");
                }

                if (pPlugin) {
                    m_plugins.push_back(pPlugin);
                    m_loadedModules.push_back({ std::move(library), pPlugin });
                }
                else {
                    SIGFLOW_LOG("PluginManager: CreateSigPlugin returned null for " + dllPath + "\n");
                    library->Unload();
                }
            }
            else {
                // 没找到导出函数，释放句柄
                SIGFLOW_LOG("PluginManager: " + dllPath +
                            " does not export CreateSigPlugin (not a SigFlow plugin?)\n");
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
