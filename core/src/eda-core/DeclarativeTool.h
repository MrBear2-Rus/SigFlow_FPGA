#pragma once

#include <eda/api/IPluginInteraction.h>
#include <eda/api/jobs.hpp>

#include <string>
#include <vector>

namespace eda {

struct DeclarativeArtifactSpec {
    std::string id;
    std::string path;   // 可含 ${param}
    std::string schema;
    std::string role = "primary";
};

// 声明式工具描述：把 CLI 调用描述为 Job（`${param}` 从 JobRequest.params 取值）。
struct DeclarativeToolSpec {
    std::string id;
    std::string jobType;
    std::string capability;
    std::string executable;                 // 可含 ${param}
    std::vector<std::string> arguments;     // 每项可含 ${param}
    std::string workingDirectory;           // 可含 ${param}；空则用 jobDir
    std::vector<std::string> requiresParams;
    std::vector<DeclarativeArtifactSpec> artifacts;
    bool requiresConfirm = false;
};

// P1-7：声明式工具运行时。解析 JSON 描述 → 直接成为一个 IJobProvider，无需为每个工具重编译。
class DeclarativeJobProvider final : public IJobProvider, public IPluginInteraction {
public:
    explicit DeclarativeJobProvider(DeclarativeToolSpec spec);

    static bool ParseSpec(const Json& json, DeclarativeToolSpec& spec, std::string& error);

    std::string jobType() const override;
    Json paramsSchema() const override;
    Json resultSchema() const override;
    bool requiresConfirm() const override;
    Error startJob(const JobRequest& request, JobContext& ctx) override;

    void invoke(const MethodCall& call, Callback<Error, Json> onReply) override;
    std::uint64_t subscribe(const std::string& topic, EventHandler<const Json&> handler) override;
    void unsubscribe(std::uint64_t subscriptionId) override;
    PluginInfo info() const override;

private:
    DeclarativeToolSpec spec_;
};

} // namespace eda
