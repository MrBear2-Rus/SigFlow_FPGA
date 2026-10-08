#pragma once

#include <wx/process.h>
#include <wx/string.h>

#include <chrono>
#include <functional>
#include <future>
#include <string>
#include <vector>

class AgentServiceController final {
public:
    enum class State {
        Disabled,
        Starting,
        Ready,
        BackingOff,
        Failed,
        Stopped,
    };

    struct Status {
        State state = State::Stopped;
        std::string message;
        int port = 0;
        unsigned int restartCount = 0;
    };

    // `executable` must be an absolute, user/admin configured sidecar launcher.
    // Secrets deliberately are not command-line arguments or environment variables:
    // they are sent once through the child stdin after it has started.
    struct Config {
        wxString executable;
        std::vector<wxString> arguments{"--bootstrap-stdin"};
        wxString dataDirectory;
        wxString workingDirectory;
        std::string instanceId;
        std::string gatewayUrl;
        std::string gatewayToken;
        std::string agentUiToken;
        std::string protocol = "edu.api.v1";
        unsigned int maxRestarts = 3;
        std::function<void(const Status&)> onStatus;
    };

    explicit AgentServiceController(Config config);
    ~AgentServiceController();

    AgentServiceController(const AgentServiceController&) = delete;
    AgentServiceController& operator=(const AgentServiceController&) = delete;

    // Starts a new sidecar lifecycle.  A missing executable is a non-fatal
    // Disabled state: the IDE and all pre-existing EDA actions remain usable.
    bool Start();
    void Stop();

    // 握手校验：与 Tick()/HandleOutputLine() 使用的规则完全一致（同一实现）。
    // 单独暴露是为了让非 GUI 测试能够拿真实 sidecar 的 ready 行验证本控制器，
    // 而不是在测试里复述一遍规则。
    struct ReadyRecord {
        int port = 0;
        std::string agentVersion;
    };
    static bool ValidateReadyLine(const std::string& line, const std::string& expectedNonce,
                                  const std::string& expectedProtocol, ReadyRecord& out,
                                  std::string& error);

    // Build the exact child argv used by Launch().  Kept deterministic so the
    // Windows .cmd/.bat installation route can be regression-tested without
    // duplicating command-interpreter rules in a test.
    static std::vector<wxString> BuildLaunchCommand(const wxString& executable,
                                                    const std::vector<wxString>& arguments);

    // Must be called on the wx main thread (MainFrame's existing 500 ms timer).
    // It consumes the bootstrap-ready line, observes exits, polls health and
    // performs bounded exponential-backoff restart.
    void Tick();

    Status status() const;
    bool ready() const { return status_.state == State::Ready; }

private:
    class SidecarProcess;

    bool Launch();
    void HandleTerminated(int exitCode);
    void DrainOutput();
    void HandleOutputLine(const std::string& line);
    void StartHealthProbe();
    void ConsumeHealthProbe();
    void ScheduleRestart(const std::string& reason);
    void Publish(State state, std::string message);
    std::string BootstrapPayload() const;

    Config config_;
    SidecarProcess* process_ = nullptr;  // self-deletes after wx delivers termination.
    long processId_ = 0;
    Status status_;
    bool desiredRunning_ = false;
    bool receivedReady_ = false;
    int healthFailures_ = 0;
    std::string nonce_;
    std::string stdoutBuffer_;
    std::string stderrTail_;
    std::chrono::steady_clock::time_point readyDeadline_{};
    std::chrono::steady_clock::time_point restartAt_{};
    std::chrono::steady_clock::time_point nextHealthAt_{};
    std::future<bool> healthProbe_;
};

