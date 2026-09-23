#include <eda/api/jobs.hpp>
#include <eda/api/process.hpp>
#include <eda/api/schemas.hpp>

#include "eda-core/JobService.h"
#include "eda-core/SchemaRegistry.h"
#include "eda-core/CoreSchemas.h"

#include <chrono>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

using namespace std::chrono_literals;

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

template <typename F>
bool WaitUntil(F predicate, int timeoutMs = 20000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) return true;
        std::this_thread::sleep_for(20ms);
    }
    return predicate();
}

std::unique_ptr<eda::IProcessHost> MakeHost() {
    return eda::CreatePlatformProcessHost();
}

eda::JobState StateOf(eda::CoreJobService& service, const std::string& id) {
    const auto record = service.get(id);
    return record ? record->state : eda::JobState::Failed;
}

class SuccessProvider final : public eda::IJobProvider {
public:
    std::string jobType() const override { return "synth"; }
    eda::Json paramsSchema() const override { return eda::Json::object(); }
    eda::Json resultSchema() const override { return eda::Json::object(); }
    eda::Error startJob(const eda::JobRequest& request, eda::JobContext& ctx) override {
        ctx.log("working", false);
        ctx.progress(50, "half");
        eda::Artifact artifact;
        artifact.id = "netlist";
        artifact.path = request.projectId + "/top.json";
        artifact.schema = "eda.netlist.yosys-json.v1";
        artifact.role = "primary";
        ctx.registerArtifact(artifact);
        ctx.emitMetric(eda::Json{{"lut", 96}});
        return eda::Error::Ok();
    }
};

class ThrowingProvider final : public eda::IJobProvider {
public:
    std::string jobType() const override { return "boom"; }
    eda::Json paramsSchema() const override { return eda::Json::object(); }
    eda::Json resultSchema() const override { return eda::Json::object(); }
    eda::Error startJob(const eda::JobRequest&, eda::JobContext&) override {
        throw std::runtime_error("provider exploded");
    }
};

class SlowProcessProvider final : public eda::IJobProvider {
public:
    explicit SlowProcessProvider(std::string type) : type_(std::move(type)) {}
    std::string jobType() const override { return type_; }
    eda::Json paramsSchema() const override { return eda::Json::object(); }
    eda::Json resultSchema() const override { return eda::Json::object(); }
    eda::Error startJob(const eda::JobRequest&, eda::JobContext& ctx) override {
        eda::ProcessSpec spec;
#ifdef _WIN32
        spec.executable = "cmd.exe";
        spec.arguments = {"/C", "ping", "-n", "30", "127.0.0.1"};
#else
        spec.executable = "/bin/sh";
        spec.arguments = {"-c", "sleep 30"};
#endif
        const eda::ProcessResult result = ctx.processHost().Run(spec, nullptr);
        ctx.log(std::string("outcome=") + std::to_string(static_cast<int>(result.outcome)), false);
        return eda::Error::Ok();
    }

private:
    std::string type_;
};

} // namespace

int main() {
    {
        eda::CoreJobService service(&MakeHost);
        service.RegisterProvider(std::make_shared<SuccessProvider>());
        eda::JobRequest request;
        request.jobType = "synth";
        request.projectId = "prj";
        const std::string id = service.submit(request);
        Check(WaitUntil([&]() { return service.report(id).artifacts.size() == 1; }),
              "job succeeded and recorded artifact");
        const eda::JobReport report = service.report(id);
        Check(report.state == eda::JobState::Succeeded, "report state Succeeded");
        Check(report.artifacts[0].id == "netlist", "report artifact id");
        Check(report.metrics.value("lut", 0) == 96, "report metric recorded");
    }
    {
        eda::CoreJobService service(&MakeHost);
        service.RegisterProvider(std::make_shared<ThrowingProvider>());
        eda::JobRequest request;
        request.jobType = "boom";
        request.projectId = "prj";
        const std::string id = service.submit(request);
        Check(WaitUntil([&]() { return StateOf(service, id) == eda::JobState::Failed; }),
              "provider exception becomes Failed");
    }
    {
        eda::CoreJobService service(&MakeHost);
        eda::JobRequest request;
        request.jobType = "missing";
        request.projectId = "prj";
        const std::string id = service.submit(request);
        Check(WaitUntil([&]() { return StateOf(service, id) == eda::JobState::Failed; }),
              "missing provider becomes Failed");
    }
    {
        eda::CoreJobService service(&MakeHost);
        service.RegisterProvider(std::make_shared<SlowProcessProvider>("proc"));
        eda::JobRequest request;
        request.jobType = "proc";
        request.projectId = "prj";
        const std::string id = service.submit(request);
        Check(WaitUntil([&]() { return StateOf(service, id) == eda::JobState::Running; }),
              "process job reaches Running");
        Check(service.cancel(id, "user"), "cancel accepted");
        Check(WaitUntil([&]() { return StateOf(service, id) == eda::JobState::Cancelled; }),
              "process job cancelled");
    }
    {
        eda::CoreJobService service(&MakeHost);
        service.SetDefaultTimeoutSeconds(1);
        service.RegisterProvider(std::make_shared<SlowProcessProvider>("slow"));
        eda::JobRequest request;
        request.jobType = "slow";
        request.projectId = "prj";
        const std::string id = service.submit(request);
        Check(WaitUntil([&]() { return StateOf(service, id) == eda::JobState::TimedOut; }),
              "process job timed out");
    }
    {
        eda::SimpleSchemaRegistry schemas;
        eda::RegisterCoreSchemas(schemas);
        std::string error;
        Check(schemas.HasSchema("eda.job.v1"), "core schema eda.job.v1 registered");
        Check(schemas.Validate("eda.job.v1",
                               eda::Json{{"job_id", "j"}, {"job_type", "synth"},
                                         {"state", "Succeeded"}},
                               error),
              "valid job record passes schema");
        Check(!schemas.Validate("eda.job.v1", eda::Json{{"job_id", "j"}}, error),
              "invalid job record rejected by schema");
    }

    std::cout << (g_failures == 0 ? "ALL PASS" : "FAILURES") << "\n";
    return g_failures == 0 ? 0 : 1;
}
