#pragma once

#include "WaveViewState.h"

#include <string>

namespace sigflow {
namespace wave {

// 波形会话（W3-07）：视口、信号列表、Marker、事件、A-B、播放头。
struct WaveSessionData {
    std::string sourcePath;
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
};

bool WaveSessionSave(const WaveSessionData& session, const std::string& path,
                     std::string& error);
bool WaveSessionLoad(const std::string& path, WaveSessionData& session,
                     std::string& error);

} // namespace wave
} // namespace sigflow
