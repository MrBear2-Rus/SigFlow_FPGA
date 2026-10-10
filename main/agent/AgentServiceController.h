#pragma once

#include <wx/process.h>
#include <wx/string.h>

#include <chrono>
#include <functional>
#include <future>
#include <string>
#include <vector>
#include <memory>

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
        std::uint64_t generation = 0;
        std::string agentVersion;
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

    // 启动材料（AD-01）：sidecar 只 `readline()` 一次，所以宿主写出的必须是
    // **恰好一行** UTF-8 JSON。字段单独抽出，使非 GUI 测试能拿到"宿主真实写出的字节"
    // 去喂真实 Python reader，而不是在测试里自己拼一份（那会掩盖本缺陷）。
    struct BootstrapFields {
        std::string protocol;
        std::string instanceId;
        std::string nonce;
        std::string gatewayUrl;
        std::string gatewayToken;
        std::string uiToken;
        std::string dataDirectory;
    };
    // 返回含结尾换行的一行 JSON；序列化器显式关闭缩进。
    static std::string SerializeBootstrapLine(const BootstrapFields& fields);
    // 本次启动真实会写出的字节（nonce 为空表示尚未 Launch）。
    std::string BootstrapLineForTest() const;

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
    // 统一收口"结束当前这一代进程"：进程已自行退出时不再调用 Kill，
    // 避免 wx 打出 "Failed to kill process (error 5: 拒绝访问)" 这类噪声。
    void KillCurrentProcess();

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
    // AD-02：ready 行只表示"进程起来了"，必须再用一次真实 health 校验才允许进入 Ready。
    // pendingPort_ 保存尚未验证的端口；generation_ 让上一代进程的探测结果失效。
    int pendingPort_ = 0;
    unsigned int generation_ = 0;
    unsigned int healthProbeGeneration_ = 0;
    std::chrono::steady_clock::time_point readyDeadline_{};
    std::chrono::steady_clock::time_point restartAt_{};
    std::chrono::steady_clock::time_point nextHealthAt_{};
    std::future<bool> healthProbe_;
    std::string pendingVersion_;
};

