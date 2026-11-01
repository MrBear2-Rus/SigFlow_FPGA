#include <eda/api/process.hpp>
#include "eda-platform/Platform.h"
#include "eda-platform/Sha256.h"
#include "eda-platform/DynamicLibrary.h"
#include "eda-core/Toolchain.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>
#include <thread>
#include <sys/stat.h>
#include <unistd.h>

namespace fs = std::filesystem;
void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
int main() {
    const auto dir = fs::temp_directory_path() / ("sigflow-platform-" + std::to_string(getpid()));
    fs::create_directories(dir);
    struct Cleanup { fs::path p; ~Cleanup() { fs::remove_all(p); } } cleanup{dir};
    try {
        check(eda::platform::Sha256Hex("abc",3) ==
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "SHA256");
        const auto unicode = dir / "工程.sv";
        check(eda::platform::PathFromUtf8(eda::platform::PathToUtf8(unicode)) == unicode, "UTF8 path");
        check(eda::platform::RelativePathToUtf8("a\\b.sv") == "a\\b.sv", "Linux filename backslash");
        eda::platform::DynamicLibrary library;
        check(library.Load("libc.so.6") && library.Symbol("getpid"), "dynamic library");
        library.Unload();
        check(!library.Load((dir / "missing.so").string()), "missing library");
        eda::ProcessSpec spec;
        spec.executable = "/bin/sh";
        spec.arguments = {"-c", "printf '%s' \"$SIGFLOW_TEST\"; printf error >&2"};
        spec.workingDirectory = dir;
        spec.environment = {{"SIGFLOW_TEST", "value with spaces;$"}};
        std::string streamed;
        auto host = eda::CreatePlatformProcessHost();
        auto r = host->Run(spec, [&](const auto& s, bool) { streamed += s; });
        check(r.outcome == eda::ProcessOutcome::Success && r.errorOutput == "error", "stdout/stderr");
        check(r.output.find("value with spaces;$") != std::string::npos && streamed == r.output, "environment and streaming");
        spec.arguments = {"-c", "printf 1234567890"}; spec.maxOutputBytes = 4;
        r = host->Run(spec, {});
        check(r.output == "1234" && r.outputTruncated, "output cap");
        spec.maxOutputBytes = 0; spec.arguments = {"-c", "exit 17"};
        r = host->Run(spec, {});
        check(r.outcome == eda::ProcessOutcome::NonZeroExit && r.exitCode == 17, "exit code");
        spec.executable = dir / "missing";
        r = host->Run(spec, {});
        check(r.outcome == eda::ProcessOutcome::LaunchFailed && !r.started, "launch failure");
        spec.executable = "/bin/sh"; spec.workingDirectory = dir / "missing";
        r = host->Run(spec, {});
        check(r.outcome == eda::ProcessOutcome::LaunchFailed, "invalid working directory");
        spec.workingDirectory = dir; spec.arguments = {"-c", "sleep 30 & wait"}; spec.timeoutSeconds = 1;
        auto start = std::chrono::steady_clock::now();
        r = host->Run(spec, {});
        check(r.outcome == eda::ProcessOutcome::TimedOut &&
              std::chrono::steady_clock::now()-start < std::chrono::seconds(5), "timeout tree cleanup");
        spec.timeoutSeconds = 0;
        host = eda::CreatePlatformProcessHost();
        std::thread cancel([&] { std::this_thread::sleep_for(std::chrono::milliseconds(200)); host->Cancel(); });
        r = host->Run(spec, {}); cancel.join();
        check(r.outcome == eda::ProcessOutcome::Cancelled, "cancel outcome");
        host = eda::CreatePlatformProcessHost();
        spec.arguments = {"-c", "sleep 30 & exit 0"};
        start = std::chrono::steady_clock::now();
        r = host->Run(spec, {});
        check(r.outcome == eda::ProcessOutcome::Success &&
              std::chrono::steady_clock::now()-start < std::chrono::seconds(5), "inherited pipes cleanup");
        auto makeTool = [&](const char* name, bool executable=true) {
            auto path = dir / name / "yosys"; fs::create_directories(path.parent_path());
            std::ofstream(path) << "#!/bin/sh\nexit 0\n";
            chmod(path.c_str(), executable ? 0700 : 0600); return path;
        };
        auto configured=makeTool("configured"), explicitRoot=makeTool("explicit"),
             env=makeTool("env"), runtime=makeTool("runtime"), pathTool=makeTool("path");
        std::map<std::string,std::string> values{{"SIGFLOW_YOSYS",env.string()},
            {"SIGFLOW_TOOL_RUNTIME_DIR",runtime.parent_path().string()}, {"PATH",pathTool.parent_path().string()}};
        eda::DefaultToolchain tools({}, [&](const auto& key) { return values[key]; });
        eda::ToolQuery query; query.name="yosys"; query.environmentVariables={"SIGFLOW_YOSYS"};
        query.configuredPath=configured.string(); query.bundledRoots={explicitRoot.parent_path()};
        check(tools.Resolve(query).path==configured, "configured precedence");
        query.configuredPath.clear(); check(tools.Resolve(query).path==explicitRoot, "tool-root precedence");
        query.bundledRoots.clear(); check(tools.Resolve(query).path==env, "environment precedence");
        values["SIGFLOW_YOSYS"].clear(); check(tools.Resolve(query).path==runtime, "runtime precedence");
        values["SIGFLOW_TOOL_RUNTIME_DIR"].clear(); check(tools.Resolve(query).path==pathTool, "PATH fallback");
        chmod(pathTool.c_str(),0600); check(!tools.Resolve(query).found, "reject non-executable");
        query.name.clear(); check(!tools.Resolve(query).found, "empty tool query");
        std::cout << "platform and toolchain checks passed\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
