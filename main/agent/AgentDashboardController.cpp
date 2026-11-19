#include "agent/AgentDashboardController.h"

#include <chrono>
#include <utility>

namespace agent {
namespace {

std::string StringField(const Json::Value& value, const char* key) {
    return value.isObject() && value[key].isString() ? value[key].asString() : std::string();
}

}  // namespace

AgentDashboardController::AgentDashboardController(Callback callback)
    : callback_(std::move(callback)) {}

AgentDashboardController::~AgentDashboardController() {
    if (worker_.valid()) worker_.wait();
}

bool AgentDashboardController::Busy() const {
    return worker_.valid() &&
           worker_.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready;
}

bool AgentDashboardController::Start(std::shared_ptr<const AgentApiClient> client,
                                     std::uint64_t generation) {
    if (!client) return false;
    if (worker_.valid()) {
        if (worker_.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
            return false;
        }
        worker_.get();
    }
    const std::uint64_t requestId = ++requestId_;
    worker_ = std::async(
        std::launch::async,
        [callback = callback_, client = std::move(client), generation, requestId]() {
            Snapshot snapshot;
            snapshot.generation = generation;
            snapshot.requestId = requestId;

            const auto usage = client->GetUsage(30);
            if (usage.ok() && usage.envelope["data"].isObject() &&
                StringField(usage.envelope["data"], "schema") == "edu.usage.v1") {
                snapshot.usageOk = true;
                snapshot.usage = usage.envelope["data"];
            } else {
                snapshot.usageErrorCode =
                    usage.errorCode.empty() ? "INVALID_RESPONSE" : usage.errorCode;
                snapshot.usageErrorMessage =
                    usage.errorMessage.empty() ? "用量响应不符合当前协议" : usage.errorMessage;
            }

            const auto balance = client->GetBalance();
            if (balance.ok() && balance.envelope["data"].isObject() &&
                StringField(balance.envelope["data"], "schema") == "edu.model.balance.v1") {
                snapshot.balanceOk = true;
                snapshot.balance = balance.envelope["data"];
            } else {
                snapshot.balanceErrorCode =
                    balance.errorCode.empty() ? "INVALID_RESPONSE" : balance.errorCode;
                snapshot.balanceErrorMessage =
                    balance.errorMessage.empty() ? "余额响应不符合当前协议" : balance.errorMessage;
            }

            if (callback) callback(snapshot);
        });
    return true;
}

}  // namespace agent
