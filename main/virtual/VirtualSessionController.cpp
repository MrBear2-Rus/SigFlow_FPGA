#include "VirtualSessionController.h"
#include "eda-virtual-board/DurableJournal.h"
#include "eda-platform/Platform.h"
#include <wx/process.h>
#include <wx/thread.h>
#include <wx/utils.h>
#include <wx/log.h>
#include <fstream>
namespace sigflow::virtual_lab {
namespace {
using eda::Json;
class Child:public wxProcess {
public:
    Child():reaper_(this){Bind(wxEVT_TIMER,[this](wxTimerEvent&){
        // A retired child may be blocked writing a trace page. Continue draining
        // without any controller/window reference, then bound abnormal shutdown.
        size_t budget=8192;char byte;for(auto* stream:{GetInputStream(),GetErrorStream()})while(budget && stream && stream->CanRead()){stream->Read(&byte,1);if(!stream->LastRead())break;--budget;}
        if(std::chrono::steady_clock::now()>=deadline_){reaper_.Stop();wxLogNull quiet;wxProcess::Kill(retired_pid_,wxSIGKILL,wxKILL_NOCHILDREN);}
    });}
    std::function<void()> done;
    void Retire(long pid){done={};retired_pid_=pid;CloseOutput();deadline_=std::chrono::steady_clock::now()+std::chrono::seconds(2);reaper_.Start(20);}
    void OnTerminate(int,int)override{reaper_.Stop();if(done)done();delete this;}
private:
    wxTimer reaper_;long retired_pid_=0;std::chrono::steady_clock::time_point deadline_;
};
uint64_t Number(const Json& j){auto s=j.get<std::string>();if(s.empty() || s.find_first_not_of("0123456789")!=std::string::npos)throw std::runtime_error("E_GUI_TRACE_NUMBER");return std::stoull(s);}
}
const char* StateName(SessionState s){switch(s){case SessionState::Unbuilt:return "Unbuilt";case SessionState::Building:return "Building";case SessionState::Ready:return "Ready";case SessionState::Running:return "Running";case SessionState::Paused:return "Paused";case SessionState::Stopped:return "Stopped";default:return "Error";}}
SessionState MapState(const Json& f){auto s=f.value("state","");if(s=="Running")return SessionState::Running;if(s=="Stopped")return SessionState::Stopped;
    if(s=="Paused")return f.value("reason","")=="Ready"?SessionState::Ready:SessionState::Paused;return SessionState::Error;}
bool CanCommand(SessionState s,const std::string& op){if(op=="replay.current" || op=="replay.file")return s==SessionState::Ready || s==SessionState::Paused || s==SessionState::Stopped;
    if(op=="pause")return s==SessionState::Running;if(op=="step")return s==SessionState::Ready || s==SessionState::Paused;
    return s==SessionState::Ready || s==SessionState::Paused || s==SessionState::Running;}
VirtualSessionController::VirtualSessionController():timer_(this),alive_(std::make_shared<std::atomic<bool>>(true)){Bind(wxEVT_TIMER,&VirtualSessionController::Tick,this);}
VirtualSessionController::~VirtualSessionController(){*alive_=false;Shutdown();}
void VirtualSessionController::Shutdown(){
    builder_.Cancel();
    timer_.Stop();queue_.clear();outstanding_=nullptr;
    // EOF lets the bounded session worker finalize and close the journal. The
    // detached completion handler owns Child until OnTerminate, without a UI
    // pointer. Killing immediately after EOF races a normally exiting process.
    if(process_){static_cast<Child*>(process_)->Retire(pid_);process_=nullptr;pid_=0;}
    if(mirror_)try{mirror_->Seal();}catch(...){}
}
void VirtualSessionController::BuildProject(Json project){
    if(!wxIsMainThread())throw std::runtime_error("E_GUI_THREAD");Shutdown();state_=SessionState::Building;error_.clear();frame_=Json::object();build_result_=Json::object();replay_result_=Json::object();Notify();
    builder_.Start(std::move(project),[this](Json result){build_result_=std::move(result);
        if(!build_result_.value("ok",false)){state_=SessionState::Error;error_="工程装配被阻止，请检查诊断";Notify();return;}
        Open(wxString::FromUTF8(build_result_.at("runner_path").get<std::string>()),wxString::FromUTF8(build_result_.at("session_config").get<std::string>()),wxString::FromUTF8(build_result_.at("working_directory").get<std::string>()));});
}
void VirtualSessionController::BuildReference(){
    try{BuildProject(VirtualProjectSessionBuilder::ReadProject(VirtualProjectSessionBuilder::FindRepository()/"examples/virtual/counter/virtual.project.json"));}
    catch(const std::exception& e){Fail(e.what());}
}
void VirtualSessionController::LocateDiagnostic(size_t index){if(diagnostic_handler_ && build_result_.contains("diagnostics") && index<build_result_["diagnostics"].size())diagnostic_handler_(build_result_["diagnostics"][index]);}
void VirtualSessionController::Replay(unsigned n,const std::string& path){if(!CanCommand(state_,"replay.current")){replay_result_={{"status","rejected"},{"error","E_REPLAY_REQUIRES_PAUSE"}};Notify();return;}replay_result_={{"status","running"}};Notify();Json j={{"op",path.empty()?"replay.current":"replay.file"},{"repetitions",n}};if(!path.empty())j["path"]=path;Queue(std::move(j));}
void VirtualSessionController::Open(const wxString& executable,const wxString& config,const wxString& cwd){
    if(!wxIsMainThread())throw std::runtime_error("E_GUI_THREAD");Shutdown();frame_=Json::object();buffer_.clear();stderr_.clear();error_.clear();targets_.clear();mirror_.reset();source_.reset();mirror_revision_=0;state_=SessionState::Building;Notify();
    try{
        std::ifstream in(eda::platform::PathFromUtf8(config.ToStdString(wxConvUTF8)));Json definition;in>>definition;
        if(!definition.contains("board_path"))throw std::runtime_error("实验会话需要显式板卡定义 board_path");
        for(const auto& input:definition.at("inputs"))targets_[input.at("package_pin").get<int>()]=input.at("target");
        auto* child=new Child();process_=child;child->Redirect();
        child->done=[this]{std::string failure;try{Drain();}catch(const std::exception& e){failure=e.what();}process_=nullptr;pid_=0;
            if(!failure.empty())Fail(failure);else if(state_!=SessionState::Stopped && state_!=SessionState::Error)Fail("会话进程已退出。"+stderr_);};
        const wchar_t* args[]={executable.wc_str(),config.wc_str(),nullptr};wxExecuteEnv env;env.cwd=cwd;
        pid_=wxExecute(args,wxEXEC_ASYNC|wxEXEC_HIDE_CONSOLE,child,&env);
        if(!pid_){process_=nullptr;delete child;throw std::runtime_error("无法启动实验会话进程");}
        Send({{"op","get"}});timer_.Start(33);
    }catch(const std::exception& e){Fail(e.what());}
}
std::string VirtualSessionController::CommandId(){return "gui-"+std::to_string(++command_);}
void VirtualSessionController::Queue(Json command){
    if(!wxIsMainThread()){auto alive=alive_;CallAfter([this,alive,command=std::move(command)]{if(*alive)Queue(command);});return;}
    if(!process_ || !CanCommand(state_,command.value("op","")))return;
    if(queue_.size()>=128){error_="操作队列已满，请等待当前操作完成";Notify();return;}
    if(!command.contains("command_id"))command["command_id"]=CommandId();queue_.push_back(std::move(command));
}
void VirtualSessionController::Send(const Json& command){
    auto text=command.dump()+"\n";auto* stream=process_->GetOutputStream();stream->Write(text.data(),text.size());
    if(!stream->IsOk())throw std::runtime_error("实验会话通道写入失败");outstanding_=command;sent_=std::chrono::steady_clock::now();
    if(command.at("op")=="get")next_snapshot_=sent_+std::chrono::milliseconds(200);
}
void VirtualSessionController::Drain(){
    if(!process_)return;char bytes[1];size_t budget=32*1024;auto deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(4);
    for(auto* stream:{process_->GetInputStream(),process_->GetErrorStream()}){
        // Pipe streams can wait for the rest of a requested buffer. Read only
        // an already available byte, with a per-UI-frame wall-time budget.
        while(stream && budget && std::chrono::steady_clock::now()<deadline && stream->CanRead()){
            stream->Read(bytes,std::min(sizeof(bytes),budget));auto count=stream->LastRead();if(!count)break;budget-=count;
            if(stream==process_->GetErrorStream()){if(stderr_.size()<16384)stderr_.append(bytes,std::min(count,16384-stderr_.size()));}
            else buffer_.append(bytes,count);
        }
    }
    if(buffer_.size()>16*1024*1024)throw std::runtime_error("E_GUI_FRAME_QUOTA");
    for(size_t end; (end=buffer_.find('\n'))!=std::string::npos;){auto line=buffer_.substr(0,end);buffer_.erase(0,end+1);Receive(Json::parse(line));}
}
void VirtualSessionController::Receive(const Json& response){
    if(outstanding_.is_null())throw std::runtime_error("E_GUI_UNSOLICITED_FRAME");auto op=outstanding_.at("op").get<std::string>();outstanding_=nullptr;
    if(op=="replay.current" || op=="replay.file"){replay_result_=response;Notify();return;}
    if(response.value("status","")=="rejected"){
        error_=response.value("error","会话拒绝操作");if(response.contains("state")){frame_=response;state_=MapState(frame_);}else if(state_==SessionState::Building)state_=SessionState::Error;
        Notify();return;
    }
    if(op=="trace.commits"){
        using namespace eda::virtual_board;
        if(!mirror_){std::vector<LiveSignal> signals;for(const auto& s:response.at("signals"))signals.push_back({s.at("id"),s.at("name"),s.at("width"),s.at("kind")});
            mirror_=std::make_shared<LiveTraceStore>(response.at("run_id"),Number(response.at("generation")),signals);mirror_->BindWriterToCurrentThread();source_=std::make_shared<trace::LiveTraceSource>(mirror_);}
        auto current=mirror_->AcquireReadView();
        if(response.at("run_id")!=current.RunId() || Number(response.at("generation"))!=current.Generation())throw std::runtime_error("E_GUI_TRACE_RUN");
        for(const auto& b:response.at("batches")){
            if(Number(b.at("revision"))!=mirror_revision_+1)throw std::runtime_error("E_GUI_TRACE_REVISION");
            std::vector<LiveTransition> events;for(const auto& e:b.at("events"))events.push_back(TraceEventFromJson(e));
            auto p=b.at("position");mirror_->AppendBatch(events);mirror_->Commit(EventPosition{Number(p.at("tick")),p.at("delta"),Number(p.at("event_seq"))});++mirror_revision_;
        }
        source_->RefreshReadView();Notify();return;
    }
    if(response.contains("state")){frame_=response;state_=MapState(frame_);error_=response.value("error","");Notify();}
}
void VirtualSessionController::Tick(wxTimerEvent&){
    try{
        Drain();if(!process_)return;
        if(!outstanding_.is_null()){
            auto timeout=outstanding_.value("op",std::string()).rfind("replay.",0)==0?120:10;
            if(std::chrono::steady_clock::now()-sent_>std::chrono::seconds(timeout))throw std::runtime_error("会话响应超时");return;
        }
        if(!queue_.empty()){auto next=queue_.front();queue_.pop_front();Send(next);return;}
        if(state_==SessionState::Running && std::chrono::steady_clock::now()>=next_snapshot_){Send({{"op","get"}});return;}
        if(frame_.contains("trace") && (!mirror_ || mirror_revision_<Number(frame_.at("trace").at("revision")))){
            Send({{"op","trace.commits"},{"after_revision",std::to_string(mirror_revision_)},{"through_revision",frame_.at("trace").at("revision")},{"limit",4}});return;
        }
        // Keep the read-only replay channel alive after Stop. Shutdown owns cleanup.
    }catch(const std::exception& e){Fail(e.what());}
}
void VirtualSessionController::Fail(const std::string& error){error_=error;state_=SessionState::Error;Shutdown();Notify();}
size_t VirtualSessionController::Subscribe(std::function<void()> f){auto id=++next_observer_;observers_[id]=std::move(f);return id;}
void VirtualSessionController::Unsubscribe(size_t id){observers_.erase(id);}
void VirtualSessionController::Notify(){
    if(notification_pending_)return;notification_pending_=true;auto alive=alive_;
    CallAfter([this,alive]{if(!*alive)return;notification_pending_=false;auto observers=observers_;for(const auto& [id,f]:observers)if(observers_.count(id))f();});
}
}
