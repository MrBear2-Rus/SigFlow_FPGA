#pragma once

#include "DebugContract.h"

#include <cstdint>
#include <string>
#include <vector>

namespace sigflow {
namespace debug {

// P0-02 会话状态集。
enum class DebugSessionState {
    Created,
    Validating,
    Building,
    Programming,
    Armed,
    Captured,
    Compared,
    Failed,
    TimedOut,
    Cancelled,
};

const char* ToString(DebugSessionState state);
bool ParseDebugSessionState(const std::string& value, DebugSessionState& state);
bool IsTerminalDebugSessionState(DebugSessionState state);
// 合法迁移表（含 Armed → TimedOut 超时路径与取消路径）。
bool IsLegalDebugTransition(DebugSessionState from, DebugSessionState to);

struct DebugSessionTransition {
    DebugSessionState state = DebugSessionState::Created;
    std::string timestamp;
    std::string operatorName;
    std::string reason;
    int exitCode = 0;
};

struct DebugSessionInfo {
    std::string id;
    std::string retryOf;
    DebugSessionState state = DebugSessionState::Created;
    std::string createdAt;
    std::string updatedAt;
    int exitCode = 0;
    std::string buildId;
    std::string protocol;
    std::uint64_t clockHz = 0;
    std::uint32_t baud = 0;
    std::uint64_t uartDivider = 0;
    double uartErrorPercent = 0.0;
    int txPin = 0;
    int rxPin = 0;
    std::uint32_t captureDepth = 0;
    std::uint32_t captureWidth = 0;
    std::string bitstreamSha256;
    std::string overlayPath;
    std::string mergedCstPath;
    std::string netlistJsonPath;
    std::string pnrJsonPath;
    std::string bitstreamPath;
    std::string resourceReportPath;
    std::string timingReportPath;
    std::string buildFingerprint;
    std::vector<std::string> toolVersions;
    std::vector<DebugSessionTransition> transitions;
};

// P0-03 会话目录布局。
struct DebugSessionPaths {
    std::string root;
    std::string overlay;
    std::string scripts;
    std::string logs;
    std::string artifacts;
    std::string reports;
    std::string manifest;
};

class DebugSessionService {
public:
    bool Create(const std::string& projectPath, const DebugContract& contract,
                DebugSessionInfo& session, std::string& error) const;
    bool Load(const std::string& projectPath, const std::string& sessionId,
              DebugSessionInfo& session, std::string& error) const;
    bool Transition(const std::string& projectPath, const std::string& sessionId,
                    DebugSessionState target, const std::string& reason,
                    int exitCode, std::string& error) const;
    bool UpdateBuildMetadata(const std::string& projectPath, const std::string& sessionId,
                             const DebugSessionInfo& metadata, std::string& error) const;
    bool ValidateBitstreamForProgramming(const std::string& projectPath,
                                         const std::string& sessionId,
                                         const std::string& bitstreamPath,
                                         std::string& error) const;
    bool Cancel(const std::string& projectPath, const std::string& sessionId,
                const std::string& reason, std::string& error) const;
    bool List(const std::string& projectPath, std::vector<DebugSessionInfo>& sessions,
              std::string& error) const;

    // P0-03 清理策略：保留最近 N 个终态会话，删除更旧的；不触碰 .sigflow/sim。
    std::size_t CleanupOldSessions(const std::string& projectPath,
                                   std::size_t keepCount,
                                   std::vector<std::string>& removedIds,
                                   std::string& error) const;

    static std::string DebugRoot(const std::string& projectPath);
    static DebugSessionPaths GetPaths(const std::string& projectPath,
                                      const std::string& sessionId);
    static std::string ManifestPathFor(const std::string& projectPath,
                                       const std::string& sessionId);
};

} // namespace debug
} // namespace sigflow
