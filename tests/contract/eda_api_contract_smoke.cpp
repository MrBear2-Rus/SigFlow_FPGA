#include <eda/api/Types.h>
#include <eda/api/IPluginInteraction.h>
#include <eda/api/capabilities.h>
#include <eda/api/jobs.hpp>
#include <eda/api/abi.h>
#include <eda/api/build_info.h>
#include <eda/api/services.hpp>
#include <eda/api/events.hpp>
#include <eda/api/schemas.hpp>
#include <eda/api/process.hpp>

#include "eda-core/EventBus.h"
#include "eda-core/SchemaRegistry.h"

#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

namespace {

int g_failures = 0;

void Check(bool ok, const char* msg) {
    if (ok) {
        std::cout << "  ok: " << msg << "\n";
    } else {
        ++g_failures;
        std::cout << "  FAIL: " << msg << "\n";
    }
}

class StubSynth final : public eda::ISynthesizer {
public:
    std::string synthesizerId() const override { return "stub"; }

    void invoke(const eda::MethodCall&, eda::Callback<eda::Error, eda::Json> cb) override {
        cb(eda::Error::Ok(), eda::Json{{"job_id", "J1"}});
    }

    std::uint64_t subscribe(const std::string&,
                            eda::EventHandler<const eda::Json&>) override {
        return 1;
    }

    void unsubscribe(std::uint64_t) override {}

    eda::PluginInfo info() const override {
        eda::PluginInfo i;
        i.id = "stub";
        return i;
    }
};

class StubProvider final : public eda::IJobProvider {
public:
    std::string jobType() const override { return "synth"; }
    eda::Json paramsSchema() const override { return eda::Json::object(); }
    eda::Json resultSchema() const override { return eda::Json::object(); }
    eda::Error startJob(const eda::JobRequest&, eda::JobContext&) override {
        return eda::Error::Ok();
    }
};

class DummyService final : public eda::Service {
public:
    explicit DummyService(int v) : value(v) {}
    int value = 0;
};

class OtherService final : public eda::Service {};

static_assert(std::is_base_of_v<eda::Service, eda::IJobService>,
              "IJobService must derive from eda::Service");

} // namespace

int main() {
    eda::Error ok;
    Check(!static_cast<bool>(ok), "default Error is false");
    Check(eda::Error::Ok().code == eda::ErrorCode::None, "Error::Ok code None");

    eda::Error bad{eda::ErrorCode::TimedOut, "t", ""};
    Check(static_cast<bool>(bad), "non-None Error is true");
    Check(std::string(eda::ToString(bad.code)) == "TimedOut", "ToString(TimedOut)");

    eda::MethodCall call;
    call.method = "synth.run";
    call.params = eda::Json{{"x", 1}};
    Check(call.params["x"] == 1, "MethodCall carries Json params");

    eda::PluginInfo info;
    info.id = "stub";
    info.abi = EDA_PLUGIN_ABI_VERSION;
    Check(info.id == "stub" && info.abi == 1u, "PluginInfo fields");

    StubSynth synth;
    Check(synth.synthesizerId() == "stub", "ISynthesizer::synthesizerId");

    bool dispatched = false;
    synth.synth_run(eda::SynthParams{"top", {"top.v"}, "baseline"},
                    [&](eda::Error e, eda::SynthResult r) {
                        dispatched = !static_cast<bool>(e) && r.jobId == "J1";
                    });
    Check(dispatched, "EDA_TYPED_METHOD dispatch + fromJson");

    eda::JobRequest req;
    req.jobType = "synth";
    req.params = eda::Json::object();
    Check(req.jobType == "synth", "JobRequest fields");

    eda::JobRecord rec;
    rec.id = "J1";
    rec.state = eda::JobState::Queued;
    Check(rec.state == eda::JobState::Queued, "JobState enum");

    StubProvider provider;
    Check(provider.jobType() == "synth", "IJobProvider stub compiles");

    Check(EDA_PLUGIN_ABI_VERSION == 1u, "ABI version macro");

    const std::string buildInfo = eda::BuildInfo();
    Check(!buildInfo.empty(), "BuildInfo() non-empty");
    std::cout << "BuildInfo: " << buildInfo << "\n";

    // --- P0-2: ServiceRegistry / Context ---
    eda::Context context;
    DummyService& added = context.services().Add<DummyService>(7);
    Check(added.value == 7, "ServiceRegistry::Add returns owned ref");
    Check(context.services().Size() == 1, "ServiceRegistry::Size");
    Check(context.services().Find<DummyService>() != nullptr, "ServiceRegistry::Find hit");
    Check(context.services().Get<DummyService>().value == 7, "ServiceRegistry::Get");
    Check(context.services().Find<OtherService>() == nullptr, "ServiceRegistry::Find miss");
    Check(context.services().Remove<DummyService>(), "ServiceRegistry::Remove");
    Check(context.services().Size() == 0, "ServiceRegistry::Size after remove");

    // --- P0-2: EventBus ---
    eda::InProcessEventBus bus;
    int received = 0;
    const eda::EventHandlerId sub =
        bus.Subscribe(eda::topics::kJobFinished,
                      [&received](const eda::Json& payload) {
                          received = payload.value("n", 0);
                      });
    Check(bus.SubscriberCount(eda::topics::kJobFinished) == 1, "EventBus subscriber count");
    bus.Publish(eda::topics::kJobFinished, eda::Json{{"n", 42}});
    Check(received == 42, "EventBus subscribe/publish");
    bus.Unsubscribe(sub);
    bus.Publish(eda::topics::kJobFinished, eda::Json{{"n", 7}});
    Check(received == 42, "EventBus unsubscribe stops delivery");
    Check(bus.SubscriberCount(eda::topics::kJobFinished) == 0, "EventBus empty after unsubscribe");

    // --- P0-2: SchemaRegistry ---
    eda::SimpleSchemaRegistry schemas;
    Check(schemas.RegisterSchema("eda.test.v1",
                                 eda::Json{{"type", "object"}, {"required", {"job_id"}}}),
          "SchemaRegistry::RegisterSchema");
    Check(schemas.HasSchema("eda.test.v1"), "SchemaRegistry::HasSchema");
    std::string error;
    Check(schemas.Validate("eda.test.v1", eda::Json{{"job_id", "J1"}}, error),
          "SchemaRegistry::Validate ok");
    Check(!schemas.Validate("eda.test.v1", eda::Json::object(), error),
          "SchemaRegistry::Validate missing required fails");
    Check(!schemas.Validate("missing.v1", eda::Json::object(), error),
          "SchemaRegistry::Validate unknown schema fails");

    // --- P0-4: IProcessHost ---
    {
#ifdef _WIN32
        const std::string shell = "cmd.exe";
        const std::vector<std::string> echoArgs = {"/C", "echo", "eda-process-ok"};
        const std::vector<std::string> sleepArgs = {"/C", "ping", "-n", "6", "127.0.0.1"};
        const std::vector<std::string> cancelArgs = {"/C", "ping", "-n", "30", "127.0.0.1"};
#else
        const std::string shell = "/bin/sh";
        const std::vector<std::string> echoArgs = {"-c", "echo eda-process-ok"};
        const std::vector<std::string> sleepArgs = {"-c", "sleep 5"};
        const std::vector<std::string> cancelArgs = {"-c", "sleep 30"};
#endif
        auto echoHost = eda::CreatePlatformProcessHost();
        eda::ProcessSpec echoSpec;
        echoSpec.executable = shell;
        echoSpec.arguments = echoArgs;
        std::string streamed;
        const eda::ProcessResult echoResult =
            echoHost->Run(echoSpec, [&streamed](const std::string& chunk, bool) {
                streamed += chunk;
            });
        Check(echoResult.started, "ProcessHost started");
        Check(echoResult.outcome == eda::ProcessOutcome::Success, "ProcessHost exit success");
        Check(echoResult.output.find("eda-process-ok") != std::string::npos,
              "ProcessHost output captured");
        Check(streamed.find("eda-process-ok") != std::string::npos,
              "ProcessHost output streamed");

        auto timeoutHost = eda::CreatePlatformProcessHost();
        eda::ProcessSpec timeoutSpec;
        timeoutSpec.executable = shell;
        timeoutSpec.arguments = sleepArgs;
        timeoutSpec.timeoutSeconds = 1;
        const eda::ProcessResult timeoutResult = timeoutHost->Run(timeoutSpec, nullptr);
        Check(timeoutResult.started, "ProcessHost timeout test started");
        Check(timeoutResult.outcome == eda::ProcessOutcome::TimedOut,
              "ProcessHost timeout outcome");

        auto cancelHost = eda::CreatePlatformProcessHost();
        eda::ProcessSpec cancelSpec;
        cancelSpec.executable = shell;
        cancelSpec.arguments = cancelArgs;
        eda::ProcessResult cancelResult;
        std::thread worker([&]() { cancelResult = cancelHost->Run(cancelSpec, nullptr); });
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        cancelHost->Cancel();
        worker.join();
        Check(cancelResult.outcome == eda::ProcessOutcome::Cancelled,
              "ProcessHost cancel outcome");
    }

    std::cout << (g_failures == 0 ? "ALL PASS" : "FAILURES") << "\n";
    return g_failures == 0 ? 0 : 1;
}
