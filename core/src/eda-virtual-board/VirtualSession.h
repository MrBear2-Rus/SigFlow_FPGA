#pragma once
#include <eda/api/port_manifest.hpp>
#include <eda/api/electrical_profile.hpp>
#include <eda/api/logic_state.hpp>
#include <eda/api/Types.h>
#include <eda/api/live_trace.hpp>
#include <functional>
#include <memory>
#include <filesystem>

namespace eda::virtual_board {
class LiveTraceStore;
class IDutAdapter {
public:
    virtual ~IDutAdapter()=default;
    virtual void WriteInput(const PortBitRef&,bool)=0;
    virtual void Evaluate(const EventPosition&)=0;
    virtual bool ReadOutput(const PortBitRef&) const=0;
    virtual void Finalize()=0;
    // External zero-delay feedback adapters request another guarded delta.
    // Verilated internal loops also use the generated bounded fatal handler.
    virtual bool NeedsSettle() const { return false; }
};
struct InputEndpoint {
    std::string target;
    int package_pin=-1;
    std::vector<Driver> fixed_drivers;
    uint8_t initial_manual_state=uint8_t(State::FLT);
};
struct SessionContract {
    PortManifest manifest;
    std::vector<PinBindingV2> bindings;
    ElectricalProfile electrical;
    ElectricalIdentity identity;
    std::vector<InputEndpoint> inputs;
    int clock_pad=52;
    uint64_t frequency_hz=27000000;
    size_t max_commits=100000, max_commands=100000, max_queue=1024;
    std::string run_id;
    std::string adapter_sha256, electrical_sha256;
    uint64_t generation=1,max_step_ticks=1000000000000ULL;
    size_t max_step_events=10000;
    uint32_t max_delta=256;
    bool clock_enabled=true;
    std::string durability="memory";
    std::filesystem::path journal_path;
    TraceLimits trace_limits;
};
// Public methods may be called by GUI/IPC threads. The private persistent worker
// creates, owns, evaluates, finalizes and destroys the model exactly once.
// Requests are serialized. Replies and journal copies are immutable snapshots.
class VirtualSession {
public:
    using Factory=std::function<std::unique_ptr<IDutAdapter>()>;
    VirtualSession(SessionContract,Factory);
    ~VirtualSession();
    VirtualSession(const VirtualSession&)=delete;
    VirtualSession& operator=(const VirtualSession&)=delete;
    // JSONL-ready protocol: op=get|set|pulse|step|play|pause|stop|journal.
    // Mutations require command_id. set: target + state (F/W/S byte).
    // pulse: target + pressed_state + released_state + duration_ticks (u64 string).
    Json Submit(Json request);
    std::shared_ptr<const LiveTraceStore> Trace() const;
    // From-zero electrical replay consumes committed transactions only. It
    // ignores intents/decisions and verifies fingerprints and every output.
    static Json ReplayCommitted(const SessionContract&,Factory,const Json& records);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
