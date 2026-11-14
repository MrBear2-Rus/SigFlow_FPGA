#pragma once
#include <eda/api/port_manifest.hpp>
#include <eda/api/electrical_profile.hpp>
#include <eda/api/logic_state.hpp>
#include <eda/api/Types.h>
#include <functional>
#include <memory>

namespace eda::virtual_board {
struct EventPosition {uint64_t tick=0;uint32_t delta=0;uint64_t event_seq=0;};
class IDutAdapter {
public:
    virtual ~IDutAdapter()=default;
    virtual void WriteInput(const PortBitRef&,bool)=0;
    virtual void Evaluate(const EventPosition&)=0;
    virtual bool ReadOutput(const PortBitRef&) const=0;
    virtual void Finalize()=0;
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
    // JSONL-ready protocol: op=get|set|step|play|pause|stop|journal.
    // Mutations require command_id. set: target + state (F/W/S byte).
    Json Submit(Json request);
    // From-zero electrical replay consumes committed transactions only. It
    // ignores intents/decisions and verifies fingerprints and every output.
    static Json ReplayCommitted(const SessionContract&,Factory,const Json& records);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
