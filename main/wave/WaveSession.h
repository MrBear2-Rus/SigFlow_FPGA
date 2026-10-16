#pragma once

#include "WaveViewState.h"
#include "WaveValueFormat.h"
#include "../trace/TraceSource.h"

#include <string>
#include <unordered_map>

namespace sigflow {
namespace wave {

struct WaveSourceIdentity {
    std::string sha256;
    std::uint64_t byteSize = 0;
    std::int64_t modifiedTicks = 0; // runtime change guard; not used for relocation identity
};

struct WaveSignalBinding {
    std::string signalKey; // complete source path, never a numeric ID or a leaf-name guess
    unsigned width = 1;
    sigflow::trace::SignalKind kind = sigflow::trace::SignalKind::Scalar;
    bool visible = true;
    std::string alias;
    std::string comment;
    WaveDisplayFormat displayFormat = WaveDisplayFormat::Binary;
};

// 波形会话（W3-07）：视口、信号列表、Marker、事件、A-B、播放头。
struct WaveSessionData {
    std::string schemaVersion = "2.0";
    std::string sourcePath;
    WaveSourceIdentity sourceIdentity;
    std::vector<WaveSignalBinding> signalBindings; // visible order, then hidden/unresolved metadata
    // Numeric fields are runtime adapters; version 2 files persist only stable bindings.
    std::vector<int> visibleSignalIds;
    sigflow::trace::TimeValue timeOffset = 0;
    sigflow::trace::TimeValue timeSpan = 1000;
    std::vector<WaveMarker> markers;
    std::vector<WaveEvent> events;
    sigflow::trace::TimeValue playhead = 0;
    bool hasPlayhead = false;
    sigflow::trace::TimeValue abA = 0;
    sigflow::trace::TimeValue abB = 0;
    bool hasAB = false;
    WaveTheme theme = WaveTheme::Dark;
    std::unordered_map<int, std::string> signalAliases;
    std::unordered_map<int, std::string> signalComments;
    std::unordered_map<int, WaveDisplayFormat> signalDisplayFormats;
    std::string uartCapturePath;
};

struct WaveSessionResolution {
    WaveSessionData session;
    std::vector<WaveSignalBinding> unresolved;
    bool sourceMatched = false;
    bool legacyMigrated = false;
};

bool WaveSourceFingerprint(const std::string& path, WaveSourceIdentity& identity,
                           std::string& error);
bool WaveSourceIdentityMatches(const WaveSourceIdentity& left, const WaveSourceIdentity& right);
bool WaveSessionBindSignals(const sigflow::trace::TraceSource& source,
                            WaveSessionData& session, std::string& error);
// A different source may reuse stable signal layout only, explicitly dropping
// all source-specific time annotations. Legacy numeric IDs always require a full identity match.
bool WaveSessionResolve(const WaveSessionData& saved, const sigflow::trace::TraceSource& source,
                        const WaveSourceIdentity& current, bool layoutOnly,
                        WaveSessionResolution& result, std::string& error);

bool WaveSessionSave(const WaveSessionData& session, const std::string& path,
                     std::string& error);
bool WaveSessionLoad(const std::string& path, WaveSessionData& session,
                     std::string& error);

} // namespace wave
} // namespace sigflow
