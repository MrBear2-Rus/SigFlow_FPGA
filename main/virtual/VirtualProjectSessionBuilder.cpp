#include "VirtualProjectSessionBuilder.h"
#include "eda-platform/Platform.h"
#include <wx/stdpaths.h>
#include <wx/filename.h>
#include <wx/utils.h>
#include <fstream>
#include <atomic>
namespace sigflow::virtual_lab {
namespace {
class BuildChild:public wxProcess {public:std::function<void(int)> done;void OnTerminate(int,int code)override{if(done)done(code);delete this;}};
eda::Json Read(const std::filesystem::path& p){std::ifstream input(p);eda::Json j;input>>j;return j;}
}
VirtualProjectSessionBuilder::VirtualProjectSessionBuilder():timer_(this){Bind(wxEVT_TIMER,&VirtualProjectSessionBuilder::Tick,this);}
VirtualProjectSessionBuilder::~VirtualProjectSessionBuilder(){Cancel();}
std::filesystem::path VirtualProjectSessionBuilder::FindRepository(){
    wxString configured;if(wxGetEnv("SIGFLOW_REPOSITORY_ROOT",&configured))return eda::platform::PathFromUtf8(configured.ToStdString(wxConvUTF8));
    for(auto candidate:{eda::platform::PathFromUtf8(wxStandardPaths::Get().GetExecutablePath().ToStdString(wxConvUTF8)).parent_path(),std::filesystem::current_path()})
        for(auto p=candidate;!p.empty();p=p.parent_path()){
            if(std::filesystem::exists(p/"tools/virtual/build_virtual_project.py") && std::filesystem::exists(p/"registry/boards/tang-nano-9k.virtual.json"))return p;
            if(p.parent_path()==p)break;
        }
    throw std::runtime_error("E_APPLICATION_ASSETS: 无法定位 registry 与生产工程构建器");
}
eda::Json VirtualProjectSessionBuilder::ReadProject(const std::filesystem::path& file){auto j=Read(file);j["project_root"]=eda::platform::PathToUtf8(std::filesystem::absolute(file).parent_path());return j;}
void VirtualProjectSessionBuilder::Cancel(){timer_.Stop();done_={};if(child_){static_cast<BuildChild*>(child_)->done={};if(pid_)wxProcess::Kill(pid_,wxSIGKILL,wxKILL_CHILDREN);child_=nullptr;pid_=0;}}
void VirtualProjectSessionBuilder::Start(eda::Json request,std::function<void(eda::Json)> callback,const std::string& tool){
    Cancel();done_=std::move(callback);
    try{
        auto repo=request.contains("repo_root")?eda::platform::PathFromUtf8(request.at("repo_root")):FindRepository();request["repo_root"]=eda::platform::PathToUtf8(repo);
        static std::atomic<uint64_t> serial{0};auto folder=repo/".work/virtual-build-jobs"/(std::to_string(wxGetProcessId())+"-"+std::to_string(++serial));std::filesystem::create_directories(folder);
        auto input=folder/"request.json";result_=folder/"result.json";std::ofstream(input)<<request.dump(2)<<'\n';
        wxString python;if(!wxGetEnv("SIGFLOW_PYTHON3",&python))python="python.exe";
        if(tool!="build_virtual_project.py" && tool!="build_virtual_tracebridge.py")throw std::runtime_error("E_BUILD_TOOL");auto script=wxString::FromUTF8(eda::platform::PathToUtf8(repo/"tools/virtual"/tool));auto in=wxString::FromUTF8(eda::platform::PathToUtf8(input));auto out=wxString::FromUTF8(eda::platform::PathToUtf8(result_));
        const wchar_t* argv[]={python.wc_str(),script.wc_str(),L"--request",in.wc_str(),L"--result",out.wc_str(),nullptr};
        auto* child=new BuildChild();child_=child;child->Redirect();child->done=[this](int code){timer_.Stop();child_=nullptr;pid_=0;auto done=std::move(done_);
            eda::Json result;try{result=Read(result_);}catch(...){result={{"ok",false},{"diagnostics",eda::Json::array({{{"code","E_BUILD_PROCESS"},{"message","工程构建进程退出，未生成结果（exit="+std::to_string(code)+"）"}}})}};}if(done)done(std::move(result));};
        wxExecuteEnv env;env.cwd=wxString::FromUTF8(eda::platform::PathToUtf8(repo));
#ifdef _WIN32
        if(!wxGetEnvMap(&env.env))throw std::runtime_error("E_BUILD_ENVIRONMENT");
        wxString parentPath;wxGetEnv("PATH",&parentPath);
        wxArrayString pathKeys;for(const auto& entry:env.env)if(entry.first.CmpNoCase("PATH")==0)pathKeys.Add(entry.first);
        for(const auto& key:pathKeys)env.env.erase(key);
        env.env["PATH"]="E:\\sigflow_fpga\\toolchain\\mingw64\\bin;E:\\sigflow_fpga\\toolchain\\posix;"+parentPath;
#endif
        pid_=wxExecute(argv,wxEXEC_ASYNC|wxEXEC_HIDE_CONSOLE,child,&env);
        if(!pid_){child_=nullptr;delete child;throw std::runtime_error("E_BUILD_PROCESS_START");}started_=std::chrono::steady_clock::now();timer_.Start(25);
    }catch(const std::exception& e){auto done=std::move(done_);if(done)done({{"ok",false},{"diagnostics",eda::Json::array({{{"code","E_BUILD_PROJECT"},{"message",e.what()}}})}});}
}
void VirtualProjectSessionBuilder::Tick(wxTimerEvent&){
    if(!child_)return;char byte;size_t budget=32768;for(auto* stream:{child_->GetInputStream(),child_->GetErrorStream()})while(budget && stream && stream->CanRead()){stream->Read(&byte,1);if(!stream->LastRead())break;--budget;}
    if(std::chrono::steady_clock::now()-started_>std::chrono::minutes(12)){auto done=std::move(done_);Cancel();if(done)done({{"ok",false},{"diagnostics",eda::Json::array({{{"code","E_BUILD_TIMEOUT"},{"message","工程构建超时，可检查 .work/virtual-project-sessions 下的工具日志"}}})}});}
}
}
