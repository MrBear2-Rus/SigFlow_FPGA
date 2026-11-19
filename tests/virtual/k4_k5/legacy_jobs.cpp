#include "eda-core/JobService.h"
#include "eda-platform/Platform.h"
#include "eda-platform/Sha256.h"
#include "VerilatorSimulator.h"
#include "VerilatorRunJob.h"
#include "VcdWaveformBackend.h"
#include <chrono>
#include <fstream>
#include <iostream>
#include <thread>
namespace fs=std::filesystem;using eda::Json;
eda::JobReport Wait(eda::CoreJobService& service,const std::string& id){
    auto until=std::chrono::steady_clock::now()+std::chrono::seconds(150);
    for(;;){auto r=service.report(id);if(r.state==eda::JobState::Succeeded)return r;
        if(r.state==eda::JobState::Failed || r.state==eda::JobState::TimedOut || r.state==eda::JobState::Cancelled)throw std::runtime_error("Legacy job failed: "+r.diagnostics.dump());
        if(std::chrono::steady_clock::now()>until){service.cancel(id,"test timeout");throw std::runtime_error("Legacy job timeout");}std::this_thread::sleep_for(std::chrono::milliseconds(10));}
}
int main(int argc,char** argv){try{
    if(argc!=4)return 2;auto root=eda::platform::PathFromUtf8(argv[1]),work=eda::platform::PathFromUtf8(argv[2]);fs::create_directories(work);
    auto out=work/("sim-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())),exe=out/"legacy-adder.exe";fs::create_directories(out);
    eda::CoreJobService service([]{return eda::CreatePlatformProcessHost();},work/"jobs");service.SetDefaultTimeoutSeconds(150);
    std::ofstream log(work/"jobs.log");std::mutex mutex;service.SetLogSink([&](const auto& text,bool){std::lock_guard lock(mutex);log<<text;log.flush();});
    service.RegisterProvider(std::make_shared<eda::sim::VerilatorSimulator>());service.RegisterProvider(std::make_shared<eda::sim::VerilatorRunJob>());
    eda::JobRequest build;build.jobType="sim.build";build.projectId="k45-legacy";Json sources=Json::array();
    for(const auto& f:{"top.v","full_adder.v","tb_full_adder.v"})sources.push_back(eda::platform::PathToUtf8(root/"examples/full_adder/src"/f));
    build.params={{"top_module","top"},{"testbench","tb"},{"source_files",sources},{"out_dir",eda::platform::PathToUtf8(out)},
        {"executable",eda::platform::PathToUtf8(exe)},{"verilator_path",argv[3]}};
    auto built=Wait(service,service.submit(build));if(built.artifacts.empty())throw std::runtime_error("No sim.build executable artifact");
    eda::JobRequest run;run.jobType="sim.run";run.projectId="k45-legacy";run.params={{"sim_exe",eda::platform::PathToUtf8(exe)},{"working_dir",eda::platform::PathToUtf8(out)}};
    auto ran=Wait(service,service.submit(run));if(ran.artifacts.empty())throw std::runtime_error("No sim.run waveform artifact");
    auto vcd=out/"waveform/wave.vcd";eda::wave::VcdWaveformBackend source;std::string error;
    if(!source.Open(eda::platform::PathToUtf8(vcd),error))throw std::runtime_error(error);
    bool sum=false,carry=false,transitions=false;
    for(const auto& signal:source.Signals())if(signal.name=="sum" || signal.name=="cout"){
        std::string last;if(!source.ValueAt(signal.id,source.TimeRange().end,last,error))throw std::runtime_error(error);
        if(signal.name=="sum" && last=="1")sum=true;if(signal.name=="cout" && last=="1")carry=true;
        std::vector<eda::WaveTransition> events;source.Query(signal.id,0,source.TimeRange().end,events,error);if(signal.name=="sum" && events.size()>=4)transitions=true;
    }
    if(!sum || !carry || !transitions)throw std::runtime_error("Real full-adder VCD truth-table regression");
    std::ofstream(work/"legacy-results.json")<<Json{{"status","passed"},{"sim_build","real PlatformProcessHost / Succeeded"},{"sim_run","real PlatformProcessHost / Succeeded"},
        {"vcd_backend","Open / Query / ValueAt"},{"full_adder_final_sum",1},{"full_adder_final_carry",1},{"runner_sha256",eda::platform::Sha256FileHex(exe)},
        {"vcd_sha256",eda::platform::Sha256FileHex(vcd)}}.dump(2)<<'\n';
    std::cout<<"PASS real CoreJobService sim.build/sim.run + real Verilator full-adder VCD Open/Query/ValueAt\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
