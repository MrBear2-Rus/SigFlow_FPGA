#pragma once

#include <memory>
#include <string>
#include <vector>

#include "eda-core/JobService.h"
#include "eda-core/PluginHost.h"

class PluginManager;
class ISigPlugin;

namespace sigflow {

// P0-8 组合器骨架：把插件的发现/加载/诊断从 MainFrame 抽出并集中管理。
// 旧路径（PluginManager + ISigPlugin，如 DeepSeek）保留，新契约（eda::PluginHost）并行接入；
// 后续 P3 再把 legacy 插件迁移到 IPluginInteraction 并删除 PluginManager。
class Composer {
public:
    Composer();
    ~Composer();

    // 加载 legacy 插件目录，并发现/校验/加载新契约插件；返回可打印的诊断报告。
    std::string LoadPlugins(const std::string& pluginDirectoryUtf8,
                            const std::string& exeDirUtf8,
                            const std::string& projectDirUtf8);

    ISigPlugin* LegacyPlugin(const std::string& name) const;
    // P2-8：按"角色"解析 legacy 插件，宿主不再硬编码插件名（迁移期过渡；P3 迁移到 IPluginInteraction）。
    ISigPlugin* LegacyAssistant() const;
    std::vector<std::string> LegacyPluginNames() const;
    std::vector<std::string> PluginNames() const;   // 新契约已就绪插件 id

    eda::PluginHost& Plugins() { return pluginHost_; }
    const eda::PluginHost& Plugins() const { return pluginHost_; }

    // P1-8：核心 Job 服务（内置工具插件已注册为 IJobProvider）。
    eda::IJobService& JobService();
    // P1-8：能力选择器数据源（已就绪插件暴露的能力 id）。
    std::vector<std::string> Capabilities() const;
    // P1-8：某能力下可用的后端（插件）列表。
    std::vector<eda::PluginInfo> Providers(const std::string& capability) const;
    // P1-8：当前默认后端（preferredId 命中优先，否则该能力首个就绪后端）。
    eda::PluginInfo DefaultProvider(const std::string& capability,
                                    const std::string& preferredId = {}) const;

    // P1-8：是否启用"经 IJobService 提交"的新路径（默认关闭，保持 legacy 行为）。
    // 开关来源：环境变量 SIGFLOW_USE_JOB_SERVICE=1。
    // GUI 行为等价性验证通过后，可把默认值改为 true。
    static bool UseJobService();
    // P1-8：经 IJobService 提交一个 Job（返回 jobId；失败返回空串）。
    std::string SubmitJob(const std::string& jobType, const std::string& projectId,
                          const eda::Json& params, bool requireConfirm,
                          std::string& error);

    // P1-8：默认后端选择的持久化（按能力存插件 id；与应用配置同层）。
    static std::string StoredDefaultProvider(const std::string& capability);
    static void StoreDefaultProvider(const std::string& capability, const std::string& pluginId);

    // 教育版 Agent：已就绪插件的 (id, version, capabilities) 快照，
    // 供 EDA Gateway 推导教育能力可用性（不暴露 PluginHost 内部）。
    struct ReadyPluginInfo {
        std::string id;
        std::string version;
        std::vector<std::string> capabilities;
    };
    std::vector<ReadyPluginInfo> ReadyPlugins() const;

private:
    std::unique_ptr<PluginManager> legacyManager_;
    eda::PluginHost pluginHost_;
    std::unique_ptr<eda::CoreJobService> jobService_;
};

} // namespace sigflow
