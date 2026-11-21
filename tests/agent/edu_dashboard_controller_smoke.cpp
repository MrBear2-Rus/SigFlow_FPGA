// Agent dashboard controller smoke test.
//
// Serves the two read-only routes over loopback and verifies that the worker
// normalizes both responses and reports protocol violations without throwing.

#include "agent/AgentApiClient.h"
#include "agent/AgentDashboardController.h"

#include <httplib.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace {

int g_failures = 0;

void Check(bool ok, const char* message) {
    if (ok) {
        std::cout << "  ok: " << message << "\n";
    } else {
        ++g_failures;
        std::cout << "  FAIL: " << message << "\n";
    }
}

std::string WriteJson(const Json::Value& value) {
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    return Json::writeString(builder, value);
}

void SendEnvelope(httplib::Response& response, const Json::Value& data) {
    Json::Value envelope(Json::objectValue);
    envelope["schema_version"] = "edu.api.v1";
    envelope["request_id"] = "req-1";
    envelope["trace_id"] = "trace-1";
    envelope["data"] = data;
    response.set_content(WriteJson(envelope), "application/json");
}

}  // namespace

int main() {
    httplib::Server server;
    std::atomic<bool> validSchema{true};

    server.Get("/api/v1/usage", [&](const httplib::Request& request, httplib::Response& response) {
        if (request.get_header_value("Authorization") != "Bearer ui-test") {
            response.status = 401;
            return;
        }
        Json::Value data(Json::objectValue);
        data["schema"] = validSchema.load() ? "edu.usage.v1" : "edu.usage.bad";
        data["generated_at"] = "2026-11-17T08:00:00Z";
        data["since"] = "2026-11-01";
        data["days"] = 30;
        data["skipped"] = 0;
        Json::Value totals(Json::objectValue);
        totals["calls"] = 4;
        totals["failed_calls"] = 1;
        totals["prompt_tokens"] = 40;
        totals["completion_tokens"] = 20;
        totals["total_tokens"] = 60;
        totals["tokens_unavailable_calls"] = 0;
        data["totals"] = totals;
        data["daily"] = Json::Value(Json::arrayValue);
        SendEnvelope(response, data);
    });

    server.Get("/api/v1/balance", [&](const httplib::Request& request, httplib::Response& response) {
        if (request.get_header_value("Authorization") != "Bearer ui-test") {
            response.status = 401;
            return;
        }
        Json::Value data(Json::objectValue);
        data["schema"] = "edu.model.balance.v1";
        data["available"] = true;
        data["checked_at"] = "2026-11-17T08:00:00Z";
        data["currency"] = "CNY";
        data["total_balance"] = "12.34";
        data["granted_balance"] = "0.00";
        data["topped_up_balance"] = "12.34";
        SendEnvelope(response, data);
    });

    const int port = server.bind_to_any_port("127.0.0.1");
    if (port <= 0) {
        std::cout << "FAIL: cannot bind loopback server\n";
        return 1;
    }
    std::thread worker([&] { server.listen_after_bind(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    AgentApiClient::Config config;
    config.host = "127.0.0.1";
    config.port = port;
    config.token = "ui-test";
    config.protocol = "edu.api.v1";
    auto client = std::make_shared<AgentApiClient>(config);

    {
        std::mutex waitMutex;
        std::condition_variable ready;
        bool done = false;
        agent::AgentDashboardController::Snapshot received;
        agent::AgentDashboardController controller(
            [&](const agent::AgentDashboardController::Snapshot& snapshot) {
                std::lock_guard<std::mutex> lock(waitMutex);
                received = snapshot;
                done = true;
                ready.notify_one();
            });
        Check(controller.Start(client, 7), "controller starts a query");
        std::unique_lock<std::mutex> lock(waitMutex);
        const bool finished = ready.wait_for(lock, std::chrono::seconds(5), [&] { return done; });
        Check(finished, "query finishes within the bound");
        if (finished) {
            Check(received.generation == 7, "generation is preserved");
            Check(received.requestId > 0, "request id is assigned");
            Check(received.usageOk, "usage response is accepted");
            Check(received.usage["totals"]["total_tokens"].asInt() == 60,
                  "usage counters are preserved");
            Check(received.balanceOk, "balance response is accepted");
            Check(received.balance["total_balance"].asString() == "12.34",
                  "balance values are preserved");
        }
    }

    validSchema.store(false);
    {
        std::mutex waitMutex;
        std::condition_variable ready;
        bool done = false;
        agent::AgentDashboardController::Snapshot received;
        agent::AgentDashboardController controller(
            [&](const agent::AgentDashboardController::Snapshot& snapshot) {
                std::lock_guard<std::mutex> lock(waitMutex);
                received = snapshot;
                done = true;
                ready.notify_one();
            });
        Check(controller.Start(client, 8), "controller starts a second query");
        std::unique_lock<std::mutex> lock(waitMutex);
        const bool finished = ready.wait_for(lock, std::chrono::seconds(5), [&] { return done; });
        Check(finished, "second query finishes within the bound");
        if (finished) {
            Check(!received.usageOk && received.usageErrorCode == "INVALID_RESPONSE",
                  "wrong usage schema is rejected");
            Check(received.balanceOk, "balance stays usable when usage is bad");
        }
    }

    server.stop();
    worker.join();
    if (g_failures != 0) {
        std::cout << g_failures << " failure(s)\n";
        return 1;
    }
    std::cout << "all checks passed\n";
    return 0;
}
