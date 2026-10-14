#pragma once

#include "CaptureRunService.h"
#include "DebugSession.h"

#include <string>
#include <vector>

namespace sigflow::debug {

enum class BuildPreflightState {
    Ready,
    SessionMissing,
    SessionIdentityMissing,
    ArtifactMissing,
    ArtifactPathMismatch,
    ArtifactStale,
    SessionNotReady,
    Failed,
};

const char* ToString(BuildPreflightState state);

struct BuildPreflightRequest {
    std::string projectPath;
    std::string sessionId;
    std::string bitstreamPath;
    std::string expectedBuildFingerprint;
    bool forProgramming = true;
};

struct BuildPreflightResult {
    BuildPreflightState state = BuildPreflightState::Failed;
    bool ready = false;
    DebugSessionInfo session;
    std::string selectedBitstream;
    std::vector<DiagnosticIssue> issues;
};

// 统一复用 DebugSessionService 的会话/路径/SHA-256 校验，避免下载入口和
// TraceBridge 向导各自判断“旧位流”。它只读会话与产物，不执行下载。
class TraceBridgeBuildPreflightService {
public:
    static bool Run(const BuildPreflightRequest& request,
                    BuildPreflightResult& result,
                    std::string& error);
};

} // namespace sigflow::debug
