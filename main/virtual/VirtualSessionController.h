#pragma once
#include <eda/api/Types.h>
#include "trace/LiveTraceSource.h"
#include <wx/event.h>
#include <wx/timer.h>
#include <atomic>
#include <chrono>
#include <deque>
#include <functional>
#include <map>
#include "VirtualProjectSessionBuilder.h"
class wxProcess;
namespace sigflow::virtual_lab {
enum class SessionState { Unbuilt,Building,Ready,Running,Paused,Stopped,Error };
const char* StateName(SessionState);
SessionState MapState(const eda::Json&);
bool CanCommand(SessionState,const std::string&);

// GUI owns only the transport/mirror. The persistent child owns DUT/eval.
// Public Queue() is safe to call from another thread; all wx access is marshalled.
class VirtualSessionController:public wxEvtHandler {
public:
    VirtualSessionController();
    ~VirtualSessionController();
    void Open(const wxString& executable,const wxString& config,const wxString& workingDirectory);
    void BuildProject(eda::Json);
    void BuildReference();
    void Replay(unsigned repetitions=3,const std::string& path={});
    const eda::Json& BuildResult()const{return build_result_;}
    const eda::Json& ReplayResult()const{return replay_result_;}
    void SetDiagnosticHandler(std::function<void(const eda::Json&)> handler){diagnostic_handler_=std::move(handler);}
    void LocateDiagnostic(size_t index);
    void Shutdown();
    void Queue(eda::Json);
    std::string CommandId();
    SessionState State()const{return state_;}
    const eda::Json& Frame()const{return frame_;}
    const std::string& Error()const{return error_;}
    const std::map<int,std::string>& InputTargets()const{return targets_;}
    std::shared_ptr<trace::LiveTraceSource> Source()const{return source_;}
    size_t Subscribe(std::function<void()>);
    void Unsubscribe(size_t);
private:
    void Tick(wxTimerEvent&);
    void Drain();
    void Send(const eda::Json&);
    void Receive(const eda::Json&);
    void Notify();
    void Fail(const std::string&);
    wxTimer timer_;
    VirtualProjectSessionBuilder builder_;
    eda::Json build_result_=eda::Json::object(),replay_result_=eda::Json::object();
    std::function<void(const eda::Json&)> diagnostic_handler_;
    wxProcess* process_=nullptr;
    long pid_=0;
    SessionState state_=SessionState::Unbuilt;
    eda::Json frame_=eda::Json::object(),outstanding_;
    std::string buffer_,stderr_,error_;
    std::deque<eda::Json> queue_;
    uint64_t command_=0,mirror_revision_=0;
    std::map<int,std::string> targets_;
    std::shared_ptr<eda::virtual_board::LiveTraceStore> mirror_;
    std::shared_ptr<trace::LiveTraceSource> source_;
    std::shared_ptr<std::atomic<bool>> alive_;
    std::map<size_t,std::function<void()>> observers_;
    size_t next_observer_=0;
    bool notification_pending_=false;
    std::chrono::steady_clock::time_point sent_;
    std::chrono::steady_clock::time_point next_snapshot_;
};
}
