#pragma once

#include <cstdint>
#include <string>

#include "Types.h"

namespace eda {

class IPluginInteraction {
public:
    virtual ~IPluginInteraction() = default;

    virtual void invoke(const MethodCall& call, Callback<Error, Json> onReply) = 0;
    virtual std::uint64_t subscribe(const std::string& topic, EventHandler<const Json&> handler) = 0;
    virtual void unsubscribe(std::uint64_t subscriptionId) = 0;
    virtual PluginInfo info() const = 0;

    virtual void onLoad() {}
    virtual void onUnload() {}
};

} // namespace eda
