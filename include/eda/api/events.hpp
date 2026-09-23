#pragma once

#include <cstdint>
#include <string>

#include "Types.h"

namespace eda {

// 事件主题（HarnessPlan §9.3 冻结）。
namespace topics {
inline constexpr const char* kProjectOpened   = "project/opened";
inline constexpr const char* kProjectChanged  = "project/changed";
inline constexpr const char* kIrDirty         = "ir/dirty";
inline constexpr const char* kIrRebuilt       = "ir/rebuilt";
inline constexpr const char* kJobCreated      = "job/created";
inline constexpr const char* kJobState        = "job/state";
inline constexpr const char* kJobOutput       = "job/output";
inline constexpr const char* kJobProgress     = "job/progress";
inline constexpr const char* kJobFinished     = "job/finished";
inline constexpr const char* kArtifactProduced = "artifact/produced";
inline constexpr const char* kPluginStatus    = "plugin/status";
inline constexpr const char* kToolchainChanged = "toolchain/changed";
inline constexpr const char* kSelectionChanged = "selection/changed";
inline constexpr const char* kAgentMessage    = "agent/message";
inline constexpr const char* kAgentDecision   = "agent/decision";
} // namespace topics

using EventHandlerId = std::uint64_t;

class EventBus {
public:
    using Handler = EventHandler<Json>;

    virtual ~EventBus() = default;
    virtual EventHandlerId Subscribe(const std::string& topic, Handler handler) = 0;
    virtual void Unsubscribe(EventHandlerId id) = 0;
    virtual void Publish(const std::string& topic, const Json& payload) = 0;
};

} // namespace eda
