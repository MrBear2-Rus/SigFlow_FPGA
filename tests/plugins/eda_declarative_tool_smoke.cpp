#include <eda/api/jobs.hpp>
#include <eda/api/process.hpp>

#include "eda-core/DeclarativeTool.h"
#include "eda-core/JobService.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

namespace fs = std::filesystem;
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

// 模拟声明式工具：把 "-o <path>" 指向的文件写出来。
class FakeDeclarativeHost final : public eda::IProcessHost {
public:
    eda::ProcessResult Run(const eda::ProcessSpec& spec,
                           const eda::ProcessOutputCallback& onOutput) override {
        if (onOutput) onOutput("declarative tool\n", false);
        for (std::size_t i = 0; i + 1 < spec.arguments.size(); ++i) {
            if (spec.arguments[i] == "-o") {
                std::ofstream(spec.arguments[i + 1], std::ios::binary) << "decl-output";
            }
        }
        eda::ProcessResult result;
        result.started = true;
        result.outcome = eda::ProcessOutcome::Success;
        result.exitCode = 0;
        return result;
    }

    void Cancel() override {}
};

} // namespace

int main() {
    const fs::path dir = fs::temp_directory_path() / "eda_declarative_tool_test";
    std::error_code cleanupError;
    fs::remove_all(dir, cleanupError);
    fs::create_directories(dir);
    const fs::path output = dir / "out.txt";

    const std::string specJson = R"({
        "id": "decl.test",
        "job_type": "decl",
        "capability": "decl/test",
        "executable": "mytool",
        "arguments": ["-o", "${output}", "${source_files}"],
        "requires_params": ["output"],
        "artifacts": [{"id": "out", "path": "${output}", "schema": "eda.test.v1"}]
    })";

    eda::DeclarativeToolSpec spec;
    std::string error;
    Check(eda::DeclarativeJobProvider::ParseSpec(eda::Json::parse(specJson), spec, error),
          "parse declarative tool spec");
    Check(spec.jobType == "decl" && spec.capability == "decl/test", "spec fields");

    {
        eda::DeclarativeToolSpec bad;
        std::string badError;
        Check(!eda::DeclarativeJobProvider::ParseSpec(eda::Json{{"id", "x"}}, bad, badError),
              "spec missing job_type/executable rejected");
    }

    eda::CoreJobService service([]() { return std::make_unique<FakeDeclarativeHost>(); });
    service.RegisterProvider(std::make_shared<eda::DeclarativeJobProvider>(spec));

    eda::JobRequest request;
    request.jobType = "decl";
    request.projectId = "prj";
    request.params = eda::Json{{"output", output.string()},
                               {"source_files", eda::Json::array({"a.v", "b.v"})}};

    const std::string id = service.submit(request);
    Check(WaitUntil([&]() {
              const eda::JobReport report = service.report(id);
              return report.state == eda::JobState::Succeeded && !report.artifacts.empty();
          }),
          "declarative job reaches Succeeded with declared artifact");
    const eda::JobReport report = service.report(id);
    Check(!report.artifacts.empty() && report.artifacts[0].id == "out",
          "declared artifact id registered");

    // 缺必填参数：伪造一个不带 output 的请求。
    {
        eda::JobRequest bad = request;
        bad.params = eda::Json{{"source_files", eda::Json::array({"a.v"})}};
        const std::string badId = service.submit(bad);
        Check(WaitUntil([&]() {
                  return service.report(badId).state == eda::JobState::Failed;
              }),
              "declarative job fails when required param missing");
    }

    fs::remove_all(dir, cleanupError);

    std::cout << (g_failures == 0 ? "ALL PASS" : "FAILURES") << "\n";
    return g_failures == 0 ? 0 : 1;
}
