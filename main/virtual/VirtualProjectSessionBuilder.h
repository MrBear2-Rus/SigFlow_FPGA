#pragma once
#include <eda/api/Types.h>
#include <wx/process.h>
#include <wx/timer.h>
#include <functional>
#include <filesystem>
#include <chrono>
namespace sigflow::virtual_lab {
// Asynchronous build transport only. All validation/code generation is production CLI.
class VirtualProjectSessionBuilder:public wxEvtHandler {
public:
    VirtualProjectSessionBuilder();
    ~VirtualProjectSessionBuilder();
    void Start(eda::Json,std::function<void(eda::Json)>,const std::string& tool="build_virtual_project.py");
    void Cancel();
    static std::filesystem::path FindRepository();
    static eda::Json ReadProject(const std::filesystem::path&);
private:
    void Tick(wxTimerEvent&);
    wxTimer timer_;
    wxProcess* child_=nullptr;
    long pid_=0;
    std::filesystem::path result_;
    std::function<void(eda::Json)> done_;
    std::chrono::steady_clock::time_point started_;
};
}
