#pragma once

#include <eda/api/Types.h>
#include <eda/api/waveform.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace eda {
namespace agent {

// SF-08：波形 backend 工厂。宿主（Composer）注入，按需打开某 artifact 的波形数据层；
// Gateway 不直接依赖 main/trace。返回空表示该路径暂不可打开。
using WaveformBackendFactory = std::function<std::shared_ptr<IWaveformBackend>()>;

// wave artifact 的绑定记录（Job 产出后由宿主登记）。
struct WaveArtifactRecord {
    std::string artifactId;   // 网关稳定 key（= Job report 的 artifact.id）
    std::string projectId;
    std::string revision;
    std::string jobId;
    std::filesystem::path path;
    std::string sha256;
    std::string schema;
};

// 单页信号列表。
struct WaveSignalPage {
    std::vector<WaveSignal> signals;
    bool hasMore = false;
    std::string nextCursor;
    std::string timescale;
    WaveTimeRange timeRange;
    std::size_t total = 0;
};

// 单信号精确查询结果（区间起点前的初值 + 区间内全部跳变）。
struct WaveSignalQueryResult {
    int signalId = -1;
    std::string initialValue;   // 区间起点前的最后值；无前值/未知为空
    bool hasInitial = false;
    std::vector<WaveTransition> transitions;
};

struct WaveQueryRange {
    std::uint64_t startTick = 0;
    std::uint64_t endTick = 0;
    std::vector<int> signalIds;
    // SF-08：本次响应的字节预算（来自 GatewayConfig::maxResponseBodyBytes）。
    // 0 表示不设预算；超预算时按信号维度显式分页，不把截断结果伪装成 exact。
    std::size_t maxBytes = 0;
};

// 查询结果完整度（保密体系：exact 表示未截断的完整精确数据）。
enum class WaveQueryCompleteness { kExact, kComplete, kPartial };

// 单页查询输出。
struct WaveQueryPage {
    std::vector<WaveSignalQueryResult> signals;
    WaveQueryCompleteness completeness = WaveQueryCompleteness::kExact;
    bool hasMore = false;
    std::string nextCursor;
    std::string timescale;
    std::size_t transitionCount = 0;
    std::size_t omittedSignals = 0;
};

// 波形 artifact 注册与惰性打开。
// 线程安全：HTTP worker 并发访问，注册/更新由宿主线程调用。
class WaveformService {
public:
    WaveformService();

    void SetBackendFactory(WaveformBackendFactory factory);

    // 登记/覆盖某 artifact；revision 变更时旧 backend 缓存被丢弃。
    void Register(const WaveArtifactRecord& record);
    // 移除某 project 的全部登记（工程关闭时调用）。
    void RemoveProject(const std::string& projectId);

    // 取登记记录；未登记返回 false。
    bool Lookup(const std::string& artifactId, WaveArtifactRecord& out) const;

    // 信号分页。cursor 为不透明字符串（内部编码已消费数量）。
    // 返回 kOk / kNotFound / kExpired / kBadCursor / kUnavailable。
    enum class Status { kOk, kNotFound, kExpired, kBadCursor, kUnavailable };

    Status ListSignals(const std::string& artifactId, const std::string& cursor,
                       std::size_t limit, WaveSignalPage& out);

    // 精确区间查询。限额：最多 16 路、10,000 跳变、2 MiB 估算载荷。
    // chunk 为已建立游标中的信号处理偏移（内部编码）。
    Status QueryRange(const std::string& artifactId, const WaveQueryRange& request,
                      const std::string& cursor, std::size_t maxSignals,
                      std::size_t maxTransitions, std::size_t maxBytes,
                      WaveQueryPage& out);

private:
    std::shared_ptr<IWaveformBackend> OpenLocked(const WaveArtifactRecord& record,
                                                 std::string& error);
    static std::string EncodeCursor(std::size_t offset);
    static bool DecodeCursor(const std::string& cursor, std::size_t& offset);

    mutable std::mutex mutex_;
    WaveformBackendFactory factory_;
    std::unordered_map<std::string, WaveArtifactRecord> records_;
    // key = artifactId；仅在打开过的 artifact 上存在。
    struct Cache {
        std::shared_ptr<IWaveformBackend> backend;
        std::string sha256;
    };
    std::unordered_map<std::string, Cache> cache_;
};

} // namespace agent
} // namespace eda
