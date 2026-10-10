#include "agent/AgentServiceController.h"

#include <httplib.h>
#include <json/json.h>

#include <wx/filefn.h>
#include <wx/filename.h>
#include <wx/stream.h>
#include <wx/utils.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <future>
#include <random>
#include <utility>
#include <thread>

namespace {

constexpr auto kReadyTimeout = std::chrono::seconds(15);
constexpr auto kHealthInterval = std::chrono::seconds(5);
constexpr std::size_t kOutputBufferLimit = 64 * 1024;
constexpr std::size_t kStderrTailLimit = 4096;

std::string RandomHex(std::size_t length) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::random_device random;
    std::string value;
    value.reserve(length);
    for (std::size_t i = 0; i < length; ++i) value.push_back(kHex[random() & 0x0f]);
    return value;
}

std::string Utf8(const wxString& value) {
    const wxScopedCharBuffer encoded = value.ToUTF8();
    return encoded.data() == nullptr ? std::string() : std::string(encoded.data());
}

bool ParseReady(const std::string& line, const std::string& expectedNonce,
                const std::string& expectedProtocol, int& port, std::string& agentVersion,
                std::string& error) {
    Json::Value root;
    Json::CharReaderBuilder builder;
    std::string parseError;
    const std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    if (!reader->parse(line.data(), line.data() + line.size(), &root, &parseError) ||
        !root.isObject()) {
        error = "sidecar did not emit a JSON ready record";
        return false;
    }
    if (!root["type"].isString() || !root["nonce"].isString() ||
        !root["protocol"].isString() || !root["agent_version"].isString()) {
        error = "invalid ready field types"; return false;
    }
    if (root.get("type", "").asString() != "ready" ||
        root.get("nonce", "").asString() != expectedNonce) {
        error = "sidecar ready nonce did not match this IDE instance";
        return false;
    }
    if (root.get("protocol", "").asString() != expectedProtocol) {
        error = "sidecar protocol is incompatible";
        return false;
    }
    if (!root["port"].isInt() || root["port"].asInt() <= 0 || root["port"].asInt() > 65535) {
        error = "sidecar ready record has an invalid port";
        return false;
    }
    const std::string version = root.get("agent_version", "").asString();
    if (version.empty()) {
        error = "sidecar ready record omitted agent_version";
        return false;
    }
    port = root["port"].asInt();
    agentVersion = version;
    return true;
}

bool ProbeHealth(int port, const std::string& token, const std::string& protocol,
                 const std::string& instance, const std::string& version) {
    httplib::Client client("127.0.0.1", port);
    client.set_connection_timeout(0, 300000);
    client.set_read_timeout(0, 500000);
    client.set_follow_location(false);
    httplib::Request request; request.method = "GET"; request.path = "/api/v1/health";
    request.headers.emplace("Authorization", "Bearer " + token);
    std::string bytes;
    request.content_receiver = [&](const char* data, std::size_t length, std::uint64_t, std::uint64_t) {
        if (length > 64 * 1024 - bytes.size()) return false;
        bytes.append(data, length); return true;
    };
    const auto response = client.send(request);
    if (!response || response->status != 200) return false;

    Json::Value root;
    Json::CharReaderBuilder builder;
    std::string parseError;
    const std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    if (!reader->parse(bytes.data(), bytes.data() + bytes.size(),
                       &root, &parseError)) {
        return false;
    }
    if (!root.isObject() || !root["data"].isObject() || !root["schema_version"].isString()) return false;
    const Json::Value& data = root["data"];
    if (!data["protocol"].isString() || !data["instance_id"].isString() || !data["agent_version"].isString()) return false;
    return root.isObject() && root.get("schema_version", "").asString() == protocol &&
           root["request_id"].isString() && !root["request_id"].asString().empty() &&
           root["trace_id"].isString() && !root["trace_id"].asString().empty() &&
           !root.isMember("error") && data.isObject() && data["ready"].isBool() &&
           data["ready"].asBool() && data.get("protocol", "").asString() == protocol &&
           data.get("instance_id", "").asString() == instance &&
           data.get("agent_version", "").asString() == version;
}

bool IsWindowsCommandScript(const wxString& executable) {
#ifdef _WIN32
    const wxString extension = wxFileName(executable).GetExt();
    return extension.CmpNoCase("cmd") == 0 || extension.CmpNoCase("bat") == 0;
#else
    (void)executable;
    return false;
#endif
}

wxString WindowsCommandInterpreter() {
#ifdef _WIN32
    // Do not rely solely on PATH: the launcher is an explicitly configured
    // local file, and cmd.exe is part of the Windows OS image.  Keep the PATH
    // fallback for unusual Windows layouts where wxGetOSDirectory() is not
    // the SystemRoot directory.
    const wxString candidate = wxGetOSDirectory() + "\\System32\\cmd.exe";
    return wxFileExists(candidate) ? candidate : wxString("cmd.exe");
#else
    return wxString();
#endif
}

} // namespace

class AgentServiceController::SidecarProcess final : public wxProcess {
public:
    explicit SidecarProcess(std::function<void(int)> terminated)
        : terminated_(std::move(terminated)) {
        Redirect();
    }

    void DetachController() { terminated_ = {}; }

    void OnTerminate(int, int status) override {
        const auto terminated = std::move(terminated_);
        if (terminated) terminated(status);
        delete this;
    }

private:
    std::function<void(int)> terminated_;
};

AgentServiceController::AgentServiceController(Config config) : config_(std::move(config)) {
    status_.state = State::Stopped;
}

AgentServiceController::~AgentServiceController() { Stop(); }

std::vector<wxString> AgentServiceController::BuildLaunchCommand(
    const wxString& executable, const std::vector<wxString>& arguments) {
    std::vector<wxString> command;
    command.reserve(arguments.size() + (IsWindowsCommandScript(executable) ? 3 : 1));
    if (IsWindowsCommandScript(executable)) {
        // wxExecute launches PE executables directly.  A .cmd/.bat file needs
        // cmd.exe as its process host; without this wrapper the documented
        // SIGFLOW_EDU_AGENT_EXECUTABLE=.cmd installation path cannot start.
        command.push_back(WindowsCommandInterpreter());
        command.push_back("/d");  // Do not execute an AutoRun command first.
        command.push_back("/c");
    }
    command.push_back(executable);
    command.insert(command.end(), arguments.begin(), arguments.end());
    return command;
}

bool AgentServiceController::Start() {
    desiredRunning_ = true;
    status_.restartCount = 0;
    if (config_.executable.empty()) {
        Publish(State::Disabled,
                "Agent sidecar is not configured (set SIGFLOW_EDU_AGENT_EXECUTABLE to enable it).");
        return false;
    }
    return Launch();
}

void AgentServiceController::Stop() {
    desiredRunning_ = false;
    if (status_.port > 0) {
        // Best-effort bounded shutdown; normal business requests are never replayed.
        httplib::Client shutdown("127.0.0.1", status_.port);
        shutdown.set_connection_timeout(0, 200000);
        shutdown.set_read_timeout(0, 300000);
        shutdown.set_write_timeout(0, 200000);
        shutdown.Post("/api/v1/shutdown", {{"Authorization", "Bearer " + config_.agentUiToken}},
                      "{}", "application/json");
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
        while (processId_ && wxProcess::Exists(static_cast<int>(processId_)) &&
               std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (healthProbe_.valid()) healthProbe_.wait();
    KillCurrentProcess();
    nonce_.clear();
    Publish(State::Stopped, "Agent sidecar stopped.");
}

AgentServiceController::Status AgentServiceController::status() const { return status_; }

bool AgentServiceController::Launch() {
    if (!desiredRunning_) return false;
    if (!wxFileExists(config_.executable)) {
        Publish(State::Failed, "Configured Agent sidecar executable does not exist.");
        return false;
    }
    if (!config_.dataDirectory.empty() &&
        !wxFileName::Mkdir(config_.dataDirectory, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL)) {
        Publish(State::Failed, "Unable to create the Agent sidecar data directory.");
        return false;
    }

    nonce_ = RandomHex(64);
    // A restarted Python process must not retain its predecessor's UI identity.
    // The Gateway token is scoped to this IDE's own lifetime; this token is scoped
    // to one concrete sidecar process and is only ever sent via its stdin bootstrap.
    config_.agentUiToken = RandomHex(64);
    receivedReady_ = false;
    healthFailures_ = 0;
    stdoutBuffer_.clear();
    stderrTail_.clear();
    // AD-02：每代进程有自己的 generation；上一代的 ready/health 结果不得影响这一代。
    ++generation_;
    status_.generation = generation_;
    pendingPort_ = 0;
    pendingVersion_.clear();

    const std::vector<wxString> command =
        BuildLaunchCommand(config_.executable, config_.arguments);
    std::vector<const wchar_t*> argv;
    argv.reserve(command.size() + 1);
    for (const wxString& argument : command) argv.push_back(argument.wc_str());
    argv.push_back(nullptr);

    const auto launchedGeneration = generation_;
    auto* child = new SidecarProcess([this, launchedGeneration](int exitCode) {
        if (launchedGeneration == generation_) HandleTerminated(exitCode);
    });
    wxExecuteEnv environment;
    environment.cwd = config_.workingDirectory.empty() ? config_.dataDirectory : config_.workingDirectory;
    const long pid = wxExecute(argv.data(), wxEXEC_ASYNC | wxEXEC_HIDE_CONSOLE, child, &environment);
    if (pid == 0) {
        child->DetachController();
        delete child;
        ScheduleRestart("Unable to start configured Agent sidecar.");
        return false;
    }

    process_ = child;
    processId_ = pid;
    wxOutputStream* input = child->GetOutputStream();
    std::string bootstrap = BootstrapPayload();
    if (input == nullptr || bootstrap.size() > 64 * 1024) {
        KillCurrentProcess();
        std::fill(bootstrap.begin(), bootstrap.end(), '\0');
        ScheduleRestart("Agent sidecar stdin bootstrap pipe is unavailable.");
        return false;
    }
    input->Write(bootstrap.data(), bootstrap.size());
    const bool written = input->LastWrite() == bootstrap.size() && input->IsOk();
    input->Sync();
    std::fill(bootstrap.begin(), bootstrap.end(), '\0');
    if (!written || !input->IsOk()) {
        KillCurrentProcess();
        ScheduleRestart("Agent bootstrap write failed.");
        return false;
    }

    readyDeadline_ = std::chrono::steady_clock::now() + kReadyTimeout;
    Publish(State::Starting, "Agent sidecar started; waiting for authenticated readiness.");
    return true;
}

void AgentServiceController::KillCurrentProcess() {
    if (process_ == nullptr) return;
    process_->DetachController();
    const int pid = static_cast<int>(processId_);
    process_ = nullptr;
    processId_ = 0;
    if (pid != 0 && wxProcess::Exists(pid)) {
        wxProcess::Kill(pid, wxSIGKILL, wxKILL_CHILDREN);
    }
}

void AgentServiceController::Tick() {
    DrainOutput();
    const auto now = std::chrono::steady_clock::now();
    if (status_.state == State::Starting && now >= readyDeadline_) {
        KillCurrentProcess();
        ScheduleRestart("Agent sidecar did not return a valid ready record before timeout.");
    }
    ConsumeHealthProbe();
    // AD-02：ready 行之后必须先通过一次真实 health，才允许进入 Ready；
    // 之后的周期性 health 只在 Ready 状态下做。
    if (!healthProbe_.valid() && now >= nextHealthAt_ &&
        (status_.state == State::Ready ||
         (status_.state == State::Starting && receivedReady_))) {
        StartHealthProbe();
    }
    if (status_.state == State::BackingOff && now >= restartAt_) Launch();
}

void AgentServiceController::HandleTerminated(int exitCode) {
    process_ = nullptr;
    processId_ = 0;
    if (!desiredRunning_) return;
    const std::string phase = receivedReady_ ? "crashed" : "exited before readiness";
    ScheduleRestart("Agent sidecar " + phase + " (exit code " + std::to_string(exitCode) + ").");
}

void AgentServiceController::DrainOutput() {
    if (process_ == nullptr) return;
    auto drain = [](wxInputStream* stream, const std::function<bool()>& available,
                    std::string& destination, std::size_t limit) {
        if (stream == nullptr) return;
        std::array<char, 4096> buffer{};
        while (available()) {
            stream->Read(buffer.data(), buffer.size());
            const std::size_t count = stream->LastRead();
            if (count == 0) break;
            destination.append(buffer.data(), count);
            if (destination.size() > limit) {
                if (limit == kOutputBufferLimit) { destination.resize(limit + 1); break; }
                destination.erase(0, destination.size() - limit);
            }
        }
    };
    drain(process_->GetInputStream(), [this]() { return process_ && process_->IsInputAvailable(); },
          stdoutBuffer_, kOutputBufferLimit);
    drain(process_->GetErrorStream(), [this]() { return process_ && process_->IsErrorAvailable(); },
          stderrTail_, kStderrTailLimit);
    if (stdoutBuffer_.size() > kOutputBufferLimit) {
        KillCurrentProcess(); ScheduleRestart("Agent stdout exceeded protocol budget."); return;
    }

    std::size_t newline = 0;
    while ((newline = stdoutBuffer_.find('\n')) != std::string::npos) {
        std::string line = stdoutBuffer_.substr(0, newline);
        stdoutBuffer_.erase(0, newline + 1);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        HandleOutputLine(line);
    }
}

void AgentServiceController::HandleOutputLine(const std::string& line) {
    if (receivedReady_) {
        KillCurrentProcess(); ScheduleRestart("Unexpected Agent stdout after ready."); return;
    }
    if (status_.state != State::Starting) return;
    ReadyRecord record;
    std::string error;
    if (!ValidateReadyLine(line, nonce_, config_.protocol, record, error)) {
        KillCurrentProcess(); ScheduleRestart("Invalid Agent ready record."); return;
    }
    receivedReady_ = true;
    nonce_.clear();
    // AD-02：这里**不**进入 Ready。ready 行只证明进程起来了；先做一次真实 health 校验，
    // 通过后才发布 Ready（否则 UI 会在服务其实不可用时显示"已就绪"）。
    pendingPort_ = record.port;
    pendingVersion_ = record.agentVersion;
    nextHealthAt_ = std::chrono::steady_clock::now();
    Publish(State::Starting, "Agent sidecar reported ready; verifying health before enabling it.");
}

bool AgentServiceController::ValidateReadyLine(const std::string& line,
                                              const std::string& expectedNonce,
                                              const std::string& expectedProtocol,
                                              ReadyRecord& out, std::string& error) {
    out = ReadyRecord();
    error.clear();
    return ParseReady(line, expectedNonce, expectedProtocol, out.port, out.agentVersion, error);
}

void AgentServiceController::StartHealthProbe() {
    const int port = pendingPort_ != 0 ? pendingPort_ : status_.port;
    const std::string token = config_.agentUiToken;
    const std::string protocol = config_.protocol;
    const auto instance = config_.instanceId;
    const auto version = pendingVersion_;
    healthProbeGeneration_ = generation_;
    healthProbe_ = std::async(std::launch::async, [port, token, protocol, instance, version]() {
        return ProbeHealth(port, token, protocol, instance, version);
    });
    nextHealthAt_ = std::chrono::steady_clock::now() + kHealthInterval;
}

void AgentServiceController::ConsumeHealthProbe() {
    if (!healthProbe_.valid() ||
        healthProbe_.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
        return;
    }
    const bool healthy = healthProbe_.get();
    // AD-02：上一代进程的探测结果必须失效，不能影响这一代的状态。
    if (healthProbeGeneration_ != generation_) return;
    if (healthy) {
        healthFailures_ = 0;
        if (status_.state == State::Starting && receivedReady_) {
            // 首次 health 通过：这时才允许对外宣布就绪。
            status_.port = pendingPort_;
            status_.agentVersion = pendingVersion_;
            Publish(State::Ready, "Agent sidecar is ready (health verified).");
        }
        return;
    }
    if (status_.state == State::Starting && receivedReady_) {
        // 首次 health 就失败：进程虽然写了 ready，但服务不可用，按失败处理并重启。
        KillCurrentProcess();
        ScheduleRestart("Agent sidecar failed its first health check.");
        return;
    }
    ++healthFailures_;
    if (healthFailures_ < 2) return;
    KillCurrentProcess();
    ScheduleRestart("Agent sidecar health check failed twice.");
}

void AgentServiceController::ScheduleRestart(const std::string& reason) {
    if (!desiredRunning_) return;
    if (status_.restartCount >= config_.maxRestarts) {
        Publish(State::Failed, reason + " Restart limit reached; existing IDE features remain available.");
        return;
    }
    ++status_.restartCount;
    const unsigned int shift = std::min(status_.restartCount - 1, 2u);
    restartAt_ = std::chrono::steady_clock::now() + std::chrono::seconds(1u << shift);
    Publish(State::BackingOff, reason + " Retrying automatically.");
}

void AgentServiceController::Publish(State state, std::string message) {
    status_.state = state;
    status_.message = std::move(message);
    if (state != State::Ready) status_.port = 0;
    if (config_.onStatus) config_.onStatus(status_);
}

std::string AgentServiceController::SerializeBootstrapLine(const BootstrapFields& fields) {
    Json::Value payload(Json::objectValue);
    payload["type"] = "sigflow-bootstrap";
    payload["protocol"] = fields.protocol;
    payload["instance_id"] = fields.instanceId;
    payload["nonce"] = fields.nonce;
    payload["gateway_url"] = fields.gatewayUrl;
    payload["gateway_token"] = fields.gatewayToken;
    payload["ui_token"] = fields.uiToken;
    payload["data_directory"] = fields.dataDirectory;

    // AD-01：JsonCpp 的 StreamWriterBuilder 默认 indentation 是制表符，直接 writeString
    // 会输出多行；而 sidecar 只 readline() 一次，它拿到的是被截断的 "{"，于是永远不 ready。
    // 这里显式关闭缩进与注释，保证输出**恰好一行**。
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    builder["commentStyle"] = "None";
    const std::string serialized = Json::writeString(builder, payload);
    // 防御：任何字段值里若混入裸换行（例如数据目录带换行），同样会破坏"一行"契约。
    std::string line;
    line.reserve(serialized.size() + 1);
    for (char character : serialized) {
        if (character == '\n' || character == '\r') {
            line.push_back(' ');
            continue;
        }
        line.push_back(character);
    }
    line.push_back('\n');
    return line;
}

std::string AgentServiceController::BootstrapLineForTest() const { return BootstrapPayload(); }

std::string AgentServiceController::BootstrapPayload() const {
    BootstrapFields fields;
    fields.protocol = config_.protocol;
    fields.instanceId = config_.instanceId;
    fields.nonce = nonce_;
    fields.gatewayUrl = config_.gatewayUrl;
    fields.gatewayToken = config_.gatewayToken;
    fields.uiToken = config_.agentUiToken;
    fields.dataDirectory = Utf8(config_.dataDirectory);
    return SerializeBootstrapLine(fields);
}
