#include "WaveCompareSession.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace sigflow::wave {

bool WaveCompareSession::ValidTrack(const WaveCompareTrack& track, std::string& error)
{
    if (track.trackId.empty()) {
        error = "compare track id is empty";
        return false;
    }
    if (track.sourcePath.empty()) {
        error = "compare track source path is empty";
        return false;
    }
    if (!track.timeRange.valid || track.timeRange.end < track.timeRange.begin) {
        error = "compare track time range is invalid";
        return false;
    }
    return true;
}

bool WaveCompareSession::SameIdentity(const WaveSourceIdentity& left,
                                     const WaveSourceIdentity& right)
{
    return left.sha256 == right.sha256 && left.byteSize == right.byteSize &&
           left.modifiedTicks == right.modifiedTicks;
}

WaveCompareTrack* WaveCompareSession::Find(const std::string& trackId)
{
    const auto it = std::find_if(tracks_.begin(), tracks_.end(),
        [&](const WaveCompareTrack& track) { return track.trackId == trackId; });
    return it == tracks_.end() ? nullptr : &*it;
}

const WaveCompareTrack* WaveCompareSession::Find(const std::string& trackId) const
{
    const auto it = std::find_if(tracks_.begin(), tracks_.end(),
        [&](const WaveCompareTrack& track) { return track.trackId == trackId; });
    return it == tracks_.end() ? nullptr : &*it;
}

void WaveCompareSession::BumpSession(WaveCompareTrack* track)
{
    ++sessionGeneration_;
    if (sessionGeneration_ == 0) sessionGeneration_ = 1;
    if (track != nullptr) {
        ++track->generation;
        if (track->generation == 0) track->generation = 1;
    }
}

bool WaveCompareSession::AddTrack(WaveCompareTrack track, std::string& error)
{
    error.clear();
    if (!ValidTrack(track, error)) return false;
    if (Find(track.trackId) != nullptr) {
        error = "compare track already exists: " + track.trackId;
        return false;
    }
    track.reference = false;
    track.generation = 1;
    if (tracks_.empty()) {
        track.reference = true;
        referenceTrackId_ = track.trackId;
    }
    tracks_.push_back(std::move(track));
    BumpSession(nullptr);
    return true;
}

bool WaveCompareSession::ReplaceTrack(WaveCompareTrack track, std::string& error)
{
    error.clear();
    if (!ValidTrack(track, error)) return false;
    WaveCompareTrack* existing = Find(track.trackId);
    if (existing == nullptr) return AddTrack(std::move(track), error);
    const bool isReference = existing->trackId == referenceTrackId_;
    const std::uint64_t nextGeneration = existing->generation == std::numeric_limits<std::uint64_t>::max()
        ? 1 : existing->generation + 1;
    track.reference = isReference;
    track.generation = nextGeneration;
    *existing = std::move(track);
    BumpSession(nullptr);
    return true;
}

bool WaveCompareSession::RemoveTrack(const std::string& trackId, std::string& error)
{
    error.clear();
    const auto it = std::find_if(tracks_.begin(), tracks_.end(),
        [&](const WaveCompareTrack& track) { return track.trackId == trackId; });
    if (it == tracks_.end()) {
        error = "compare track not found: " + trackId;
        return false;
    }
    const bool removedReference = it->trackId == referenceTrackId_;
    tracks_.erase(it);
    if (removedReference) {
        referenceTrackId_.clear();
        if (!tracks_.empty()) {
            referenceTrackId_ = tracks_.front().trackId;
            tracks_.front().reference = true;
        }
    }
    BumpSession(nullptr);
    return true;
}

bool WaveCompareSession::SetReference(const std::string& trackId, std::string& error)
{
    error.clear();
    WaveCompareTrack* selected = Find(trackId);
    if (selected == nullptr) {
        error = "compare track not found: " + trackId;
        return false;
    }
    if (referenceTrackId_ == trackId) return true;
    for (WaveCompareTrack& track : tracks_) track.reference = false;
    selected->reference = true;
    referenceTrackId_ = trackId;
    BumpSession(nullptr);
    return true;
}

bool WaveCompareSession::SetTimeOffset(const std::string& trackId,
                                       std::int64_t offset, std::string& error)
{
    error.clear();
    WaveCompareTrack* track = Find(trackId);
    if (track == nullptr) {
        error = "compare track not found: " + trackId;
        return false;
    }
    if (track->timeOffset == offset) return true;
    track->timeOffset = offset;
    BumpSession(track);
    return true;
}

WaveCompareToken WaveCompareSession::IssueToken(const std::string& trackId) const
{
    WaveCompareToken token;
    const WaveCompareTrack* track = Find(trackId);
    if (track == nullptr) return token;
    token.trackId = track->trackId;
    token.sourcePath = track->sourcePath;
    token.sourceIdentity = track->sourceIdentity;
    token.sessionGeneration = sessionGeneration_;
    token.trackGeneration = track->generation;
    return token;
}

bool WaveCompareSession::AcceptToken(const WaveCompareToken& token) const
{
    if (token.trackId.empty() || token.sessionGeneration != sessionGeneration_) return false;
    const WaveCompareTrack* track = Find(token.trackId);
    return track != nullptr && track->generation == token.trackGeneration &&
           track->sourcePath == token.sourcePath && SameIdentity(track->sourceIdentity, token.sourceIdentity);
}

bool WaveCompareSession::TranslateTime(const std::string& trackId,
                                       sigflow::trace::TimeValue sourceTime,
                                       sigflow::trace::TimeValue& displayTime) const
{
    const WaveCompareTrack* track = Find(trackId);
    if (track == nullptr) return false;
    if (track->timeOffset >= 0) {
        const auto offset = static_cast<sigflow::trace::TimeValue>(track->timeOffset);
        if (sourceTime > std::numeric_limits<sigflow::trace::TimeValue>::max() - offset) return false;
        displayTime = sourceTime + offset;
        return true;
    }
    const auto offset = static_cast<sigflow::trace::TimeValue>(-(track->timeOffset + 1)) + 1;
    if (sourceTime < offset) return false;
    displayTime = sourceTime - offset;
    return true;
}

WaveCompareSnapshot WaveCompareSession::Snapshot() const
{
    WaveCompareSnapshot snapshot;
    snapshot.tracks = tracks_;
    snapshot.referenceTrackId = referenceTrackId_;
    snapshot.sessionGeneration = sessionGeneration_;
    return snapshot;
}

} // namespace sigflow::wave
