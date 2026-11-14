// Explicit adapter for the reference fixture, not a general adapter generator.
// All session, guard, clock, queue and journal logic is production Core code.
#include "Vk2_counter.h"
#include "verilated.h"
#include "fixture_support.hpp"
#include "eda-virtual-board/PortManifestBuilder.h"
#include "eda-platform/Platform.h"
#include <iostream>
#include <thread>
double sc_time_stamp(){return 0.0;}
namespace vb=eda::virtual_board;
class VerilatedCounter final:public vb::IDutAdapter {
    std::thread::id owner_=std::this_thread::get_id();
    VerilatedContext context_;
    std::unique_ptr<Vk2_counter> model_;
    bool finalized_=false;
    void Owner()const{if(owner_!=std::this_thread::get_id())throw std::runtime_error("E_MODEL_THREAD");}
public:
    VerilatedCounter(){context_.threads(1);model_=std::make_unique<Vk2_counter>(&context_);}
    ~VerilatedCounter(){Owner();}
    void WriteInput(const vb::PortBitRef& r,bool value)override{
        Owner();if(r.hdl_index || r.bit_slot)throw std::runtime_error("E_ADAPTER_REF");
        if(r.port_id=="k2_counter::clock_in")model_->clock_in=value;
        else if(r.port_id=="k2_counter::reset_n")model_->reset_n=value;
        else if(r.port_id=="k2_counter::key_n")model_->key_n=value;
        else throw std::runtime_error("E_ADAPTER_REF");
    }
    void Evaluate(const vb::EventPosition& p)override{Owner();context_.time(p.tick);model_->eval();}
    bool ReadOutput(const vb::PortBitRef& r)const override{
        Owner();if(r.port_id!="k2_counter::led_n" || r.bit_slot>=6 || r.hdl_index!=int64_t(r.bit_slot))throw std::runtime_error("E_ADAPTER_REF");
        return (model_->led_n>>r.bit_slot)&1;
    }
    void Finalize()override{Owner();if(finalized_)throw std::runtime_error("E_FINAL_TWICE");finalized_=true;model_->final();}
};
int main(int argc,char** argv){
    try{
        if(argc!=3)throw std::runtime_error("root export-sidecar required");
        auto root=eda::platform::PathFromUtf8(argv[1]);std::ifstream f(eda::platform::PathFromUtf8(argv[2]));eda::Json j;f>>j;
        auto manifest=vb::PortManifestBuilder::Deserialize(j.at("manifest"));if(!manifest.ok())throw std::runtime_error("manifest");
        auto contract=k2fixture::Contract(root,manifest.manifest);
        contract.adapter_sha256=eda::platform::Sha256FileHex(eda::platform::PathFromUtf8(argv[0]));
        auto factory=[]{return std::make_unique<VerilatedCounter>();};vb::VirtualSession session(contract,factory);
        for(std::string line;std::getline(std::cin,line);){
            try{
                auto command=eda::Json::parse(line);eda::Json result;
                if(command.at("op")=="replay")result=vb::VirtualSession::ReplayCommitted(contract,factory,command.at("records"));
                else result=session.Submit(command);
                std::cout<<result.dump()<<std::endl;
            }catch(const std::exception& e){std::cout<<eda::Json{{"status","rejected"},{"error",e.what()}}.dump()<<std::endl;}
        }
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
