#pragma once

// Background reader for the model dashboard.  It performs one bounded
// usage + balance query and delivers a snapshot to the host callback.  The
// callback runs on the worker thread; the GUI host marshals it back to the
// main thread and is responsible for discarding stale generations.

#include "agent/AgentApiClient.h"

#include <json/json.h>

#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <string>

namespace agent {

class AgentDashboardController final
{
public:
    struct Snapshot
    {
        std::uint64_t generation = 0;
        std::uint64_t requestId = 0;
        bool usageOk = false;
        Json::Value usage;
        std::string usageErrorCode;
        std::string usageErrorMessage;
        bool balanceOk = false;
        Json::Value balance;
        std::string balanceErrorCode;
        std::string balanceErrorMessage;
    };

    using Callback = std::function<void(const Snapshot&)>;

    explicit AgentDashboardController(Callback callback);
    ~AgentDashboardController();

    AgentDashboardController(const AgentDashboardController&) = delete;
    AgentDashboardController& operator=(const AgentDashboardController&) = delete;

    /// Start one query.  Returns false when no client is available or the
    /// previous query is still running; it never queues a second request.
    bool Start(std::shared_ptr<const AgentApiClient> client, std::uint64_t generation);

    bool Busy() const;

private:
    Callback callback_;
    mutable std::future<void> worker_;
    std::uint64_t requestId_ = 0;
};

}  // namespace agent
