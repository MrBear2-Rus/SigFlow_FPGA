#include "TraceBridgeBuildPreflight.h"

#include <algorithm>
#include <filesystem>

namespace sigflow::debug {
namespace {

DiagnosticIssue Issue(const std::string& code, const std::string& observed,
                      const std::string& action,
                      DiagnosticSeverity severity = DiagnosticSeverity::Blocking)
{
    DiagnosticIssue issue;
    issue.code = code;
    issue.stage = "build_preflight";
    issue.severity = severity;
    issue.observed = observed;
    issue.action = action;
    return issue;
}

BuildPreflightState ClassifyValidationError(const std::string& error)
{
    if (error.find("SHA-256 mismatch") != std::string::npos)
        return BuildPreflightState::ArtifactStale;
    if (error.find("not ready for programming") != std::string::npos)
        return BuildPreflightState::SessionNotReady;
    if (error.find("no recorded bitstream") != std::string::npos ||
        error.find("does not belong") != std::string::npos)
        return BuildPreflightState::ArtifactMissing;
    return BuildPreflightState::Failed;
}

} // namespace

const char* ToString(BuildPreflightState state)
{
    switch (state) {
        case BuildPreflightState::Ready: return "Ready";
        case BuildPreflightState::SessionMissing: return "SessionMissing";
        case BuildPreflightState::SessionIdentityMissing: return "SessionIdentityMissing";
        case BuildPreflightState::ArtifactMissing: return "ArtifactMissing";
        case BuildPreflightState::ArtifactPathMismatch: return "ArtifactPathMismatch";
        case BuildPreflightState::ArtifactStale: return "ArtifactStale";
        case BuildPreflightState::SessionNotReady: return "SessionNotReady";
        case BuildPreflightState::Failed: return "Failed";
    }
    return "Failed";
}

bool TraceBridgeBuildPreflightService::Run(const BuildPreflightRequest& request,
                                           BuildPreflightResult& result,
                                           std::string& error)
{
    error.clear();
    result = BuildPreflightResult();
    if (request.projectPath.empty() || request.sessionId.empty()) {
        result.state = BuildPreflightState::SessionMissing;
        result.issues.push_back(Issue("SESSION_REFERENCE_MISSING",
                                      "project path and session id are required",
                                      "select the TraceBridge session before programming"));
        error = "project path and session id are required";
        return false;
    }

    DebugSessionService sessions;
    if (!sessions.Load(request.projectPath, request.sessionId, result.session, error)) {
        result.state = BuildPreflightState::SessionMissing;
        result.issues.push_back(Issue("SESSION_MISSING", error,
                                      "reload the session or create a new debug build"));
        return false;
    }
    if (result.session.buildId.empty() || result.session.buildId != result.session.id) {
        result.state = BuildPreflightState::SessionIdentityMissing;
        result.issues.push_back(Issue("BUILD_ID_MISSING",
                                      "debug session has no valid build identity",
                                      "build a new TraceBridge bitstream before programming"));
        error = "debug session has no valid build identity";
        return false;
    }
    if (!request.expectedBuildFingerprint.empty() &&
        result.session.buildFingerprint != request.expectedBuildFingerprint) {
        result.state = BuildPreflightState::ArtifactStale;
        result.issues.push_back(Issue("BUILD_FINGERPRINT_MISMATCH",
                                      "selected session build fingerprint does not match the requested build",
                                      "rebuild the debug overlay and select its resulting session"));
        error = "debug build fingerprint mismatch";
        return false;
    }
    if (request.forProgramming && result.session.state != DebugSessionState::Programming &&
        result.session.state != DebugSessionState::Armed) {
        result.state = BuildPreflightState::SessionNotReady;
        result.issues.push_back(Issue("SESSION_NOT_READY",
                                      "debug session is " + std::string(ToString(result.session.state)),
                                      "complete the build and arm/program transition before downloading"));
        error = "debug session is not ready for programming";
        return false;
    }

    result.selectedBitstream = request.bitstreamPath.empty()
                                   ? result.session.bitstreamPath
                                   : request.bitstreamPath;
    if (result.selectedBitstream.empty() || result.session.bitstreamPath.empty()) {
        result.state = BuildPreflightState::ArtifactMissing;
        result.issues.push_back(Issue("BITSTREAM_MISSING",
                                      "debug session has no recorded bitstream",
                                      "build a matching .fs artifact before programming"));
        error = "debug session has no recorded bitstream";
        return false;
    }
    std::error_code ec;
    const auto expected = std::filesystem::weakly_canonical(result.session.bitstreamPath, ec);
    if (ec) {
        result.state = BuildPreflightState::ArtifactMissing;
        result.issues.push_back(Issue("BITSTREAM_PATH_UNRESOLVED",
                                      "recorded bitstream path could not be resolved",
                                      "rebuild the debug session artifact"));
        error = "unable to resolve recorded debug bitstream path";
        return false;
    }
    ec.clear();
    const auto selected = std::filesystem::weakly_canonical(result.selectedBitstream, ec);
    if (ec || expected != selected) {
        result.state = BuildPreflightState::ArtifactPathMismatch;
        result.issues.push_back(Issue("BITSTREAM_PATH_MISMATCH",
                                      "selected bitstream does not belong to this debug session",
                                      "select the bitstream recorded by the active session"));
        error = "selected bitstream does not belong to debug build " + result.session.buildId;
        return false;
    }
    if (!std::filesystem::is_regular_file(selected, ec)) {
        result.state = BuildPreflightState::ArtifactMissing;
        result.issues.push_back(Issue("BITSTREAM_MISSING",
                                      "selected debug bitstream file is missing",
                                      "rebuild the debug bitstream"));
        error = "selected debug bitstream file is missing";
        return false;
    }
    if (result.session.bitstreamSha256.empty()) {
        result.state = BuildPreflightState::SessionIdentityMissing;
        result.issues.push_back(Issue("BITSTREAM_HASH_MISSING",
                                      "debug session has no bitstream SHA-256",
                                      "record the final artifact hash before programming"));
        error = "debug session has no bitstream SHA-256";
        return false;
    }

    if (request.forProgramming) {
        std::string validationError;
        if (!sessions.ValidateBitstreamForProgramming(request.projectPath,
                                                      request.sessionId,
                                                      result.selectedBitstream,
                                                      validationError)) {
            result.state = ClassifyValidationError(validationError);
            result.issues.push_back(Issue(
                result.state == BuildPreflightState::ArtifactStale ? "BITSTREAM_STALE" :
                    (result.state == BuildPreflightState::SessionNotReady ? "SESSION_NOT_READY" :
                     "BITSTREAM_VALIDATION_FAILED"),
                validationError,
                "rebuild or reselect the artifact that belongs to the active session"));
            error = validationError;
            return false;
        }
    }
    result.state = BuildPreflightState::Ready;
    result.ready = true;
    if (result.session.buildFingerprint.empty()) {
        result.issues.push_back(Issue("BUILD_FINGERPRINT_UNAVAILABLE",
                                      "session has a verified bitstream hash but no build fingerprint",
                                      "record source/toolchain fingerprint on the next build",
                                      DiagnosticSeverity::Warning));
    }
    return true;
}

} // namespace sigflow::debug
