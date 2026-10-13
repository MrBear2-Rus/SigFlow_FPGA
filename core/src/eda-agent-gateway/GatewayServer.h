#pragma once

#include <eda/api/Types.h>
#include <eda/api/jobs.hpp>

#include "GatewayDto.h"
#include "ArtifactService.h"
#include "DesignContextService.h"
#include "JobEventSource.h"
#include "ReceiptStore.h"
#include "SnapshotService.h"
#include "WaveformService.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace eda {
namespace agent {

// 能力提供者：由宿主注入（如 Composer 读取 PluginHost 记录），避免 Gateway 依赖 wx。
using ReadyPluginProvider = std::function<std::vector<ReadyPlugin>()>;

// SF-03：Job 服务桥（由宿主注入 Composer::JobService()）；Gateway 不直接依赖 main/。
// 返回 IJobService* 允许为空（未接作时 Job 路由返回 503）。
using JobServiceProvider = std::function<IJobService*()>;

class EventStore;  // 前置声明（h 不暴露实现）

// 由 UI/工程线程生成的不可变快照状态。HTTP worker 只复制该 DTO。
struct GatewayConfig {
    std::string instanceId;
    std::string edition = "edu";
    std::string build;
    std::string protocolVersion = "edu.api.v1";
    std::string bindAddress = "127.0.0.1";
    int port = 0;  // 0 = 系统分配
    std::string token;  // Agent→EDA Bearer token；空则不做鉴权（仅限最小脚手架）
    // UI 专用控制身份（grant/receipt 签发）。与 Agent token 分离，Agent token 禁止签发。
    std::string uiToken;
    // SF-02 安全边界。
    std::size_t maxRequestBodyBytes = 1024 * 1024;   // 请求体 1 MiB
    // 普通响应 2 MiB 限额；超限返回 429 RESOURCE_EXHAUSTED（不静默截断 JSON）。
    std::size_t maxResponseBodyBytes = 2 * 1024 * 1024;
    bool enforceHostHeader = true;                    // 拒绝非 loopback Host
    bool enforceOrigin = true;                        // 拒绝非预期 Origin（无 Origin 视为本机直连，放行）
    // 事件 JSONL 落盘路径（空=不落盘）；Start 时若文件存在则重放其内容。
    std::string eventLogPath;
    // Grant 索引路径（通常 <project>/.sigflow/agent/grants.json；空=内存模式）。
    std::string grantStorePath;
};

// 由 UI/工程线程生成的不可变快照状态。HTTP worker 只复制该 DTO。
struct ProjectSnapshotState {
    SnapshotRequest request;
    Json sourceSummary = Json::array();
    std::string policyVersion;
    std::string bufferHash;
    // Host-only tool locations, never serialized into context/snapshot/card DTOs.
    Json trustedToolPaths = Json::object();
};

// 最小 EDA Gateway：绑定 127.0.0.1，提供 GET /api/v1/health、GET /api/v1/capabilities。
// HTTP worker 只访问不可变 DTO / 线程安全 provider，不触碰 wx。
class GatewayServer {
public:
    GatewayServer(GatewayConfig config, ReadyPluginProvider provider);
    ~GatewayServer();

    GatewayServer(const GatewayServer&) = delete;
    GatewayServer& operator=(const GatewayServer&) = delete;

    // 启动监听；成功返回 true（port 由 Port() 查询实际值）。
    bool Start(std::string& error);
    // 在后台线程运行阻塞监听循环（Start 后调用）。
    void RunAsync();
    void Stop();

    int Port() const;
    bool Running() const;

    // 工程打开/切换时由宿主设置 <project>/.sigflow/agent/grants.json。
    // 加载会使旧进程遗留的 active grant 失效，仅保留审计信息。
    bool ConfigureGrantPersistence(const std::string& path, std::string& error);

    // SF-03：注入 Job 服务桥（应在 Start 之前调用）。
    void SetJobServiceProvider(JobServiceProvider provider);

    // SF-08：注入波形 backend 工厂（应在 Start 之前调用）。
    void SetWaveformBackendFactory(WaveformBackendFactory factory);
    // SF-08：宿主在仿真 Job 产出 wave artifact 后登记（HTTP worker 可并发读取）。
    void RegisterWaveArtifact(const WaveArtifactRecord& record);
    // 工程关闭时清理该工程的 wave 登记。
    void RemoveWaveProject(const std::string& projectId);

    // SF-06：宿主在 Job 产出 artifact 后登记（只读元数据 / 受限内容）。
    void RegisterArtifact(const ArtifactRecord& record);

    // SF-05：清理快照。活跃引用（agent job 绑定 + active grant + 当前 revision 的快照）
    // 固定保留；其余按 keepRecent/maxAgeSeconds 清理。返回删除数量。
    // nowEpoch 可注入"当前时间"以便确定性审计/测试；0 表示使用真实 UTC 时间。
    bool PruneProjectSnapshots(const std::string& projectId, std::size_t keepRecent,
                               std::uint64_t maxAgeSeconds, std::size_t& removed,
                               std::string& error, std::uint64_t nowEpoch = 0);

    // UI 线程在工程打开、保存、同步完成或 dirty 状态变化时更新。
    bool UpdateProjectSnapshotState(const ProjectSnapshotState& state, std::string& error);
    // UI 线程调用：从 sigflow.project 与登记源文件构造 DTO，并持久化单调 revision。
    bool RefreshProjectSnapshotStateFromDisk(const std::filesystem::path& projectRoot, bool dirty,
                                             bool synchronized, std::string& projectId,
                                             std::string& error);
    bool MarkProjectDirty(const std::string& projectId, bool dirty,
                          const std::string& bufferHash = {});
    void RemoveProjectSnapshotState(const std::string& projectId);
    bool ProjectState(const std::string& projectId, ProjectSnapshotState& out) const;

    // NG-05：局部设计证据。宿主在 UI/设计模型线程上抽取不可变 DTO 后注入；
    // 图形/源码映射不可靠时必须由宿主给出 Ambiguous/Unavailable，不允许由 Gateway 猜测。
    void SetDesignNodes(const std::string& projectId, const std::string& revision,
                        std::vector<DesignNodeRecord> nodes);
    // 选择上下文（含未保存缓冲片段）。缓冲只用于解释，永不进入 snapshot 或工具执行。
    void UpdateSelectionContext(const std::string& projectId, const SelectionContext& selection);
    void ClearDesignContext(const std::string& projectId);

    // SF-02：追加一个事件（宿主在 project/job/artifact/selection 变化时调用）。
    void PublishEvent(const std::string& type, const std::string& projectId,
                      const std::string& traceId, const eda::Json& data);

    // NG-08：可信事件源。宿主只报告"事实"，事件由 Gateway 生成，避免 UI 直接拼事件体。
    // 全部幂等：同一事实重复上报不会重复投递。
    void NotifyProjectOpened(const std::string& projectId);
    // 保存后调用：投递 project/saved；revision 真的前进时额外投递 revision/advanced。
    void NotifyProjectSaved(const std::string& projectId);
    // 关工程/切工程前调用：投递 project/closed，然后清理该工程的运行时状态。
    void NotifyProjectClosed(const std::string& projectId);

    // NG-08：快照保留调度。宿主按固定间隔调用（例如与事件轮询同一个 500 ms 定时器，
    // 但内部按 minIntervalSeconds 节流）。活跃引用（Agent Job 绑定、active grant、
    // 当前 revision 的快照）固定保留；report 里给出每个工程的结果，便于审计。
    struct RetentionResult {
        std::size_t projectsConsidered = 0;
        std::size_t snapshotsRemoved = 0;
        bool skippedByInterval = false;
        Json report = Json::array();
    };
    RetentionResult RunSnapshotRetention(std::size_t keepRecent, std::uint64_t maxAgeSeconds,
                                         std::uint64_t minIntervalSeconds = 300,
                                         std::uint64_t nowEpoch = 0);

    // SF-02 事件源订阅：轮询已登记归属的 Agent Job，把**状态变化**转成规范事件投递
    // （job/state-changed，终态额外 job/finished）。同 (job,state) 不重复投递，可安全重复调用。
    // 宿主应在启动后按固定间隔（或收到 Job 通知时）调用；返回本次新投递的事件数。
    // 完全 wx 无关：宿主可在工作线程调用。
    std::size_t PollJobEvents();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace agent
} // namespace eda
