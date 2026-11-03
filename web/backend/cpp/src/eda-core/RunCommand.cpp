#include "RunCommand.h"
#include "JobPaths.h"
#include "eda-platform/Platform.h"
#include <charconv>
#include <map>
#include <set>
#include <fstream>
#include <iostream>
#include <climits>
#include <stdexcept>
#include <sstream>
#include <cerrno>
#include <poll.h>
#include <unistd.h>
namespace eda::web {
namespace {
bool identifier(const std::string& s) {
    if (s.empty() || s.size() > 128 || s == "." || s == "..")
        return false;
    for (unsigned char ch : s)
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
              ch == '-' || ch == '_' || ch == '.'))
            return false;
    return true;
}
std::uint64_t number(const std::string& s) {
    std::uint64_t value = 0;
    auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), value);
    if (ec != std::errc() || end != s.data() + s.size())
        throw std::invalid_argument("invalid nonnegative integer");
    return value;
}
Json parameters(std::istream& stream) {
    constexpr size_t limit = 2 * 1024 * 1024;
    std::string data;
    char buffer[4096];
    while (stream) {
        stream.read(buffer, sizeof(buffer));
        auto size = stream.gcount();
        if (data.size() + size > limit)
            throw std::invalid_argument("params exceed 2 MiB");
        data.append(buffer, size);
    }
    if (stream.bad())
        throw std::invalid_argument("cannot read params");
    try {
        auto value = Json::parse(data);
        if (!value.is_object())
            throw std::invalid_argument("params must be an object");
        return value;
    } catch (const Json::exception&) {
        throw std::invalid_argument("invalid params JSON");
    }
}
// poll rather than blocking std::istream::read so cancellation is observed even
// when the producer keeps stdin open after sending only part of the JSON.
Json parametersFromStdin(const std::function<bool()>& cancelled) {
    constexpr std::size_t limit = 2 * 1024 * 1024;
    std::string data;
    char buffer[4096];
    for (;;) {
        if (cancelled && cancelled())
            throw std::invalid_argument("cancelled while reading params");
        pollfd descriptor{STDIN_FILENO, POLLIN, 0};
        const int ready = poll(&descriptor, 1, 100);
        if (ready < 0 && errno == EINTR)
            continue;
        if (ready < 0 || (descriptor.revents & (POLLERR | POLLNVAL)))
            throw std::invalid_argument("cannot read params");
        if (ready == 0)
            continue;
        const auto count = read(STDIN_FILENO, buffer, sizeof(buffer));
        if (count < 0 && errno == EINTR)
            continue;
        if (count < 0)
            throw std::invalid_argument("cannot read params");
        if (count == 0)
            break;
        if (data.size() + static_cast<std::size_t>(count) > limit)
            throw std::invalid_argument("params exceed 2 MiB");
        data.append(buffer, static_cast<std::size_t>(count));
    }
    std::istringstream stream(data);
    return parameters(stream);
}
}  // namespace
int RunCommand(int argc, char** argv, const ProviderFactory& factory, const EventSink& sink,
               const std::function<bool()>& cancelled, const std::string& defaultDataDir,
               const std::string& defaultToolRoot) {
    const std::map<std::string, std::string> plugins = {
        {"synth", "eda-synth-yosys"},       {"pnr", "eda-pnr-nextpnr"},
        {"pack", "eda-pack-gowin"},         {"flash", "eda-program-openfpgaloader"},
        {"sim.build", "eda-sim-verilator"}, {"sim.run", "eda-sim-verilator"}};
    const std::set<std::string> allowed = {
        "--job-type", "--work-dir",    "--job-id",           "--project",  "--project-id",
        "--params",   "--timeout-sec", "--max-output-bytes", "--data-dir", "--tool-root"};
    std::map<std::string, std::string> options;
    for (int i = 2; i < argc; ++i) {
        std::string key = argv[i];
        if (!allowed.count(key) || options.count(key))
            throw std::invalid_argument("unknown or duplicate option: " + key);
        if (i + 1 == argc || std::string(argv[i + 1]).starts_with("--"))
            throw std::invalid_argument("missing option value: " + key);
        options[key] = argv[++i];
        if (options[key].empty())
            throw std::invalid_argument("empty option value: " + key);
    }
    for (const auto& key :
         {"--job-type", "--work-dir", "--job-id", "--project", "--project-id", "--params"})
        if (!options.count(key))
            throw std::invalid_argument(std::string("missing required option: ") + key);
    if (!plugins.count(options.at("--job-type")))
        throw std::invalid_argument("unknown job type");
    if (!identifier(options.at("--job-id")) || !identifier(options.at("--project-id")))
        throw std::invalid_argument("invalid Job/project ID");
    ExecutionRequest input;
    input.jobId = options.at("--job-id");
    input.request.jobType = options.at("--job-type");
    input.request.pluginId = plugins.at(input.request.jobType);
    input.request.projectId = options.at("--project-id");
    input.projectPath = std::filesystem::absolute(options.at("--project")).lexically_normal();
    // Check unnormalized paths first to reject parent traversal rather than silently normalizing
    // it.
    input.projectPath =
        CheckedPath(input.projectPath, std::filesystem::absolute(options.at("--project")), true);
    input.workDir =
        CheckedPath(input.projectPath, std::filesystem::absolute(options.at("--work-dir")), true);
    auto expected = input.projectPath / ".sigflow/agent/jobs" / input.request.jobType / input.jobId;
    if (input.workDir != expected || !std::filesystem::is_directory(input.workDir))
        throw std::invalid_argument("Job path does not match identity");
    if (options.count("--timeout-sec")) {
        auto n = number(options.at("--timeout-sec"));
        if (n > INT_MAX)
            throw std::invalid_argument("timeout overflow");
        input.request.timeoutSec = static_cast<int>(n);
    }
    if (options.count("--max-output-bytes"))
        input.maxOutputBytes = number(options.at("--max-output-bytes"));
    auto dataEnvironment = platform::EnvUtf8("SIGFLOW_DATA_DIR");
    auto toolEnvironment = platform::EnvUtf8("SIGFLOW_TOOL_RUNTIME_DIR");
    input.dataDir = options.count("--data-dir")
                        ? options.at("--data-dir")
                        : (!dataEnvironment.empty() ? dataEnvironment : defaultDataDir);
    input.toolRoot = options.count("--tool-root")
                         ? options.at("--tool-root")
                         : (!toolEnvironment.empty() ? toolEnvironment : defaultToolRoot);
    input.request.metadata["tool_root_source"] =
        options.count("--tool-root") ? "argument"
                                     : (!toolEnvironment.empty() ? "environment" : "default");
    if (options.at("--params") == "-")
        input.request.params = parametersFromStdin(cancelled);
    else {
        std::ifstream stream(options.at("--params"), std::ios::binary);
        if (!stream)
            throw std::invalid_argument("cannot open params file");
        input.request.params = parameters(stream);
    }
    auto provider = factory(input.request.jobType);
    if (!provider) {
        sink(Json{{"type", "diagnostic"},
                  {"severity", "error"},
                  {"code", "NOT_IMPLEMENTED"},
                  {"message", "provider not migrated: " + input.request.jobType},
                  {"file", ""},
                  {"line", 0}});
        return 3;
    }
    return ExecuteJob(input, *provider, sink, cancelled);
}
}  // namespace eda::web
