#pragma once

#include "WaveSession.h"

#include <cstdint>
#include <string>
#include <vector>

namespace sigflow::wave {

struct WaveCompareTrack {
    std::string trackId;
    std::string captureId;
    std::string sourcePath;
    WaveSourceIdentity sourceIdentity;
    sigflow::trace::TraceTimeRange timeRange;
    std::int64_t timeOffset = 0;
    bool reference = false;
    std::uint64_t generation = 0;
};

struct WaveCompareToken {
    std::string trackId;
    std::string sourcePath;
    WaveSourceIdentity sourceIdentity;
    std::uint64_t sessionGeneration = 0;
    std::uint64_t trackGeneration = 0;
};

struct WaveCompareSnapshot {
    std::vector<WaveCompareTrack> tracks;
    std::string referenceTrackId;
    std::uint64_t sessionGeneration = 0;
};

// Board-independent dual-track model for TB-13-06. It deliberately owns no
// UI or TraceSource objects: callers can bind each track to a separate view,
// while tokens prevent stale asynchronous work from mutating a new window.
class WaveCompareSession {
public:
    bool AddTrack(WaveCompareTrack track, std::string& error);
    bool ReplaceTrack(WaveCompareTrack track, std::string& error);
    bool RemoveTrack(const std::string& trackId, std::string& error);
    bool SetReference(const std::string& trackId, std::string& error);
    bool SetTimeOffset(const std::string& trackId, std::int64_t offset,
                       std::string& error);

    WaveCompareToken IssueToken(const std::string& trackId) const;
    bool AcceptToken(const WaveCompareToken& token) const;

    bool TranslateTime(const std::string& trackId,
                       sigflow::trace::TimeValue sourceTime,
                       sigflow::trace::TimeValue& displayTime) const;
    WaveCompareSnapshot Snapshot() const;

private:
    static bool ValidTrack(const WaveCompareTrack& track, std::string& error);
    static bool SameIdentity(const WaveSourceIdentity& left,
                             const WaveSourceIdentity& right);
    WaveCompareTrack* Find(const std::string& trackId);
    const WaveCompareTrack* Find(const std::string& trackId) const;
    void BumpSession(WaveCompareTrack* track);

    std::vector<WaveCompareTrack> tracks_;
    std::string referenceTrackId_;
    std::uint64_t sessionGeneration_ = 1;
};

} // namespace sigflow::wave
