#pragma once

#include <eda/api/Types.h>

#include "GatewayDto.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace eda {
namespace agent {

// 能力提供者：由宿主注入（如 Composer 读取 PluginHost 记录），避免 Gateway 依赖 wx。
using ReadyPluginProvider = std::function<std::vector<ReadyPlugin>()>;

struct GatewayConfig {
    std::string instanceId;
    std::string edition = "edu";
    std::string build;
    std::string protocolVersion = "edu.api.v1";
    std::string bindAddress = "127.0.0.1";
    int port = 0;  // 0 = 系统分配
    std::string token;  // Agent→EDA Bearer token；空则不做鉴权（仅限最小脚手架）
};

// 最小 EDA Gateway：绑定 127.0.0.1，提供 GET /api/v1/health、GET /api/v1/capabilities。
// HTTP worker 只访问不可变 DTO / 线程安全 provider，不触碰 wx。
class GatewayServer {
public:
    GatewayServer(GatewayConfig config, ReadyPluginProvider provider);
    ~GatewayServer();

    GatewayServer(const GatewayServer&) = delete;
    GatewayServer& operator=(const GatewayServer&) = delete;

    // 启动监听；成功返回 true（port 由 Port() 查询实际值）。
    bool Start(std::string& error);
    // 在后台线程运行阻塞监听循环（Start 后调用）。
    void RunAsync();
    void Stop();

    int Port() const;
    bool Running() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace agent
} // namespace eda
