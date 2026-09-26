#include "GatewayServer.h"

#include "GatewayDto.h"

#include <eda/api/build_info.h>

#include <httplib.h>

#include <atomic>
#include <thread>
#include <utility>

namespace eda {
namespace agent {
namespace {

std::string MakeRequestId() {
    static std::atomic<unsigned long> sequence{0};
    return "req-" + std::to_string(sequence.fetch_add(1) + 1);
}

std::string MakeTraceId() {
    static std::atomic<unsigned long> sequence{0};
    return "trace-" + std::to_string(sequence.fetch_add(1) + 1);
}

bool IsAuthorized(const httplib::Request& req, const std::string& token) {
    if (token.empty()) return true;  // 最小脚手架：未配置 token 时放行（正式必须配置）
    const std::string expected = "Bearer " + token;
    return req.get_header_value("Authorization") == expected;
}

} // namespace

struct GatewayServer::Impl {
    GatewayConfig config;
    ReadyPluginProvider provider;
    std::unique_ptr<httplib::Server> server;
    std::thread worker;
    std::atomic<bool> running{false};
    int boundPort = 0;
};

GatewayServer::GatewayServer(GatewayConfig config, ReadyPluginProvider provider)
    : impl_(std::make_unique<Impl>()) {
    impl_->config = std::move(config);
    impl_->provider = std::move(provider);
}

GatewayServer::~GatewayServer() { Stop(); }

bool GatewayServer::Start(std::string& error) {
    if (impl_->running.load()) {
        error = "gateway already running";
        return false;
    }
    impl_->server = std::make_unique<httplib::Server>();
    impl_->server->set_read_timeout(25, 0);
    impl_->server->set_write_timeout(25, 0);

    const GatewayConfig config = impl_->config;
    ReadyPluginProvider provider = impl_->provider;

    const auto send = [](httplib::Response& res, const Json& body) {
        res.set_content(body.dump(), "application/json; charset=utf-8");
    };

    impl_->server->Get("/api/v1/health", [config, send](const httplib::Request& req,
                                                        httplib::Response& res) {
        const Json envelope = SuccessEnvelope(
            MakeRequestId(), MakeTraceId(),
            BuildHealthData(config.instanceId, config.protocolVersion, config.edition,
                            config.build.empty() ? BuildInfo() : config.build));
        res.status = 200;
        send(res, envelope);
    });

    impl_->server->Get("/api/v1/capabilities",
                       [config, provider, send](const httplib::Request& req,
                                                httplib::Response& res) {
        if (!IsAuthorized(req, config.token)) {
            res.status = 401;
            send(res, FailureEnvelope(MakeRequestId(), MakeTraceId(), "UNAUTHENTICATED",
                                      "missing or invalid bearer token", false));
            return;
        }
        const std::vector<ReadyPlugin> ready = provider ? provider() : std::vector<ReadyPlugin>{};
        const std::vector<CapabilityStatus> capabilities = ResolveEducationCapabilities(ready);
        const Json envelope = SuccessEnvelope(
            MakeRequestId(), MakeTraceId(),
            BuildCapabilitiesData(config.instanceId, config.protocolVersion, config.edition,
                                  config.build.empty() ? BuildInfo() : config.build,
                                  capabilities));
        res.status = 200;
        send(res, envelope);
    });

    // 未知路径统一返回 404 失败信封。
    impl_->server->set_error_handler([send](const httplib::Request&,
                                            httplib::Response& res) {
        if (!res.body.empty()) return;  // 已有应答
        const std::string code = res.status == 404 ? "NOT_FOUND"
                                                  : "INVALID_ARGUMENT";
        send(res, FailureEnvelope(MakeRequestId(), MakeTraceId(), code,
                                  "status " + std::to_string(res.status), false));
    });

    impl_->boundPort = impl_->server->bind_to_any_port(config.bindAddress);
    if (impl_->boundPort <= 0) {
        error = "failed to bind " + config.bindAddress + " (any port)";
        impl_->server.reset();
        return false;
    }
    impl_->running.store(true);
    return true;
}

void GatewayServer::RunAsync() {
    if (!impl_->running.load() || !impl_->server) return;
    impl_->worker = std::thread([this]() { impl_->server->listen_after_bind(); });
}

void GatewayServer::Stop() {
    if (impl_->server) {
        impl_->server->stop();
    }
    if (impl_->worker.joinable()) {
        impl_->worker.join();
    }
    impl_->running.store(false);
    impl_->server.reset();
}

int GatewayServer::Port() const { return impl_->boundPort; }

bool GatewayServer::Running() const { return impl_->running.load(); }

} // namespace agent
} // namespace eda
