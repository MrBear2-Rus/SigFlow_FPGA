// Test-owned isolated project only. Credentials arrive on stdin, never argv/stdout.
#include "eda-agent-gateway/GatewayServer.h"
#include "eda-core/JobService.h"
#include "eda-core/PluginHost.h"
#include "eda-core/Toolchain.h"
#include "eda-platform/Platform.h"
#include "VerilatorSimulator.h"
#include "VcdWaveformBackend.h"
#include <eda/api/process.hpp>
#include <filesystem>
#include <iostream>
#include <string>

extern "C" void eda_plugin_force_link_YosysSynthesizer();
extern "C" void eda_plugin_force_link_VerilatorSimulator();
extern "C" void eda_plugin_force_link_VerilatorRunJob();

int main() {
    try {
        std::string line;
        // Bound credential bootstrap without retaining arbitrary input.
        for (char c; std::cin.get(c);) {
            if (c == '\n') break;
            if (line.size() >= 16384) return 2;
            line += c;
        }
        const auto input = eda::Json::parse(line);
        if (!input.is_object() || (input.size() != 5 && input.size() != 6)) return 2;
        for (const auto* key : {"project_path", "instance_id", "agent_token", "ui_token", "nonce"})
            if (!input.contains(key) || !input[key].is_string() || input[key].get<std::string>().empty()) return 2;
        if (input.size() == 6 && (!input.contains("simulation") || !input["simulation"].is_boolean())) return 2;
        const bool simulation = input.value("simulation", false);
        const auto root = eda::platform::PathFromUtf8(input["project_path"].get<std::string>());
        if (!root.is_absolute() || !std::filesystem::is_regular_file(root / "sigflow.project")) return 2;
        eda::agent::GatewayConfig config;
        config.instanceId = input["instance_id"].get<std::string>();
        config.token = input["agent_token"].get<std::string>();
        config.uiToken = input["ui_token"].get<std::string>();
        if (config.token == config.uiToken) return 2;
        config.edition = "edu";
        config.port = 0;
#ifdef _WIN32
        auto yosys = std::filesystem::path(SIGFLOW_REPO_ROOT) / "external/fpga-tools/runtime/yosys/bin/yosys.exe";
        if (!std::filesystem::is_regular_file(yosys)) return 3;
        _putenv_s("SIGFLOW_YOSYS", eda::platform::PathToUtf8(yosys).c_str());
#else
        eda::ToolQuery query;
        query.name = "yosys";
        query.searchPath = true;
        const auto tool = eda::DefaultToolchain().Resolve(query);
        if (!tool.found) return 3;
        const auto yosys = tool.path;
        setenv("SIGFLOW_YOSYS", eda::platform::PathToUtf8(yosys).c_str(), 1);
#endif
        eda::ProcessSpec probe;
        probe.executable = eda::platform::PathToUtf8(yosys);
        probe.arguments = {"-V"};
        const auto tested = eda::CreatePlatformProcessHost()->Run(probe, nullptr);
        if (tested.outcome != eda::ProcessOutcome::Success || tested.exitCode != 0) return 3;
        // Providers outlive the Core workers and all their running jobs.
        eda::PluginHost plugins;
        eda_plugin_force_link_YosysSynthesizer();
        if (simulation) {
            eda_plugin_force_link_VerilatorSimulator();
            eda_plugin_force_link_VerilatorRunJob();
            eda::ToolQuery simQuery;
            simQuery.name = "verilator_bin";
            simQuery.alternativeNames = {"verilator_bin_dbg", "verilator"};
            simQuery.environmentVariables = {"SIGFLOW_VERILATOR"};
            simQuery.searchPath = true;
            const auto simTool = eda::DefaultToolchain().Resolve(simQuery);
            std::string reason;
            if (!simTool.found || !eda::sim::CheckVerilatorToolchain(simTool.path, reason)) return 3;
        }
        plugins.RegisterBuiltins();
        eda::CoreJobService jobs([] { return eda::CreatePlatformProcessHost(); }, root / "jobs");
        jobs.SetDefaultTimeoutSeconds(90);
        std::vector<eda::agent::ReadyPlugin> ready;
        for (const auto& record : plugins.Records()) {
            if (record.status != eda::PluginStatus::Ready) continue;
            auto* provider = dynamic_cast<eda::IJobProvider*>(plugins.Get(record.id));
            if (!provider || (provider->jobType() != "synth" &&
                !(simulation && (provider->jobType() == "sim.build" || provider->jobType() == "sim.run")))) continue;
            jobs.RegisterProvider(std::shared_ptr<eda::IJobProvider>(provider, [](auto*) {}));
            eda::agent::ReadyPlugin item;
            item.id = record.id; item.version = record.version;
            item.capabilities = {provider->jobType()}; item.ready = true;
            ready.push_back(item);
        }
        if (ready.empty()) return 3;
        eda::agent::GatewayServer server(config, [ready] { return ready; });
        if (simulation) server.SetWaveformBackendFactory([] {
            return std::make_shared<eda::wave::VcdWaveformBackend>();
        });
        server.SetJobServiceProvider([&jobs]() -> eda::IJobService* { return &jobs; });
        std::string projectId, error;
        if (!server.RefreshProjectSnapshotStateFromDisk(root, false, true, projectId, error)) return 4;
        if (!server.Start(error)) return 4;
        server.RunAsync();
        std::cout << eda::Json{{"type", "ready"}, {"protocol", "edu.api.v1"},
            {"instance_id", config.instanceId}, {"nonce", input["nonce"]},
            {"project_id", projectId}, {"port", server.Port()}}.dump() << std::endl;
        // Only the owning test parent holds stdin. Refresh the fixed project;
        // never accept a caller-supplied path or expose this over Agent HTTP.
        int exitCode = 0;
        for (;;) {
            line.clear();
            bool ended = false;
            for (char c; std::cin.get(c);) {
                if (c == '\n') { ended = true; break; }
                if (line.size() >= 4096) { exitCode = 5; break; }
                line += c;
            }
            if (exitCode != 0 || (!std::cin && line.empty())) break;
            if (!ended) { exitCode = 5; break; }
            const auto command = eda::Json::parse(line, nullptr, false);
            if (!command.is_object() || command.size() != 1 ||
                !command.contains("type") || command["type"] != "refresh-project") {
                exitCode = 5; break;
            }
            std::string refreshedId;
            if (!server.RefreshProjectSnapshotStateFromDisk(root, false, true, refreshedId, error) ||
                refreshedId != projectId) { exitCode = 5; break; }
        }
        // EOF stops acceptance before cancelling owned jobs.
        server.Stop();
        for (const auto& job : jobs.list(projectId))
            jobs.cancel(job.id, "integration host shutdown");
        return exitCode;
    } catch (...) {
        std::cerr << "u2-gateway: initialization_or_runtime_failed\n";
        return 2;
    }
}


