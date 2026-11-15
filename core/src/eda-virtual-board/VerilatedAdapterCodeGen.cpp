#include "VerilatedAdapterCodeGen.h"
#include "PortManifestBuilder.h"
#include <sstream>
#include <set>
namespace eda::virtual_board {
namespace {
bool Identifier(const std::string& s){
    auto letter=[](char c){return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||c=='_';};
    if(s.empty() || !letter(s[0]))return false;
    for(char c:s)if(!letter(c) && !(c>='0'&&c<='9'))return false;
    static const std::set<std::string> reserved={"class","struct","template","operator","this","new","delete","private","public","protected","default","switch","case","return","if","else","for","while","do","bool","int","long","short","float","double","char","void","auto","const","volatile","static","namespace","using","union","enum","true","false","nullptr","virtual","override","final","throw","try","catch","typename","signed","unsigned","sizeof","alignof","and","or","not","xor","bitand","bitor","compl","requires","concept","co_await","co_yield","co_return"};
    return !reserved.count(s);
}
}
AdapterGeneration VerilatedAdapterCodeGen::Generate(const PortManifest& manifest,const std::string& top){
    AdapterGeneration r;auto checked=PortManifestBuilder::Deserialize(PortManifestBuilder::Serialize(manifest));
    if(!checked.ok()){r.diagnostics=checked.diagnostics;return r;}
    if(!Identifier(top)){r.diagnostics.push_back({"E_ADAPTER_IDENTIFIER","Unsupported top C++ class","top_class"});return r;}
    for(const auto& p:manifest.ports){
        if(!Identifier(p.rtl_name) || p.rtl_name.rfind("__",0)==0){r.diagnostics.push_back({"E_ADAPTER_IDENTIFIER","Unsupported/mangled C++ member: "+p.rtl_name,"ports"});return r;}
        if(p.width>64 || p.direction==Direction::InOut){r.diagnostics.push_back({"E_ADAPTER_UNSUPPORTED","M0 supports scalar/packed <=64-bit input/output with no implicit OE","ports"});return r;}
    }
    std::ostringstream s;
    s<<"// Generated from validated SigFlow PortManifest; no module-specific glue.\n#include \""<<top<<".h\"\n#include \"verilated.h\"\n#include \"eda-virtual-board/RunnerSupport.h\"\n#include <thread>\n#include <stdexcept>\n#include <cstdint>\n#include <type_traits>\n";
    s<<"double sc_time_stamp(){return 0.0;}\nvoid vl_fatal(const char*,int,const char*,const char* message){std::string m=message?message:\"Verilated fatal\";throw std::runtime_error((m.find(\"converge\")!=std::string::npos?\"E_NON_CONVERGENT: \":\"E_MODEL_FATAL: \")+m);}\nnamespace vb=eda::virtual_board;\n";
    s<<"class GeneratedAdapter final:public vb::IDutAdapter {\nstd::thread::id owner_=std::this_thread::get_id();\nVerilatedContext context_;\nstd::unique_ptr<"<<top<<"> model_;\nbool finalized_=false;\nvoid Owner()const{if(owner_!=std::this_thread::get_id())throw std::runtime_error(\"E_MODEL_THREAD\");}\npublic:\n";
    s<<"static vb::PortManifest Manifest(){auto r=vb::PortManifestBuilder::Deserialize(eda::Json::parse("<<Json(PortManifestBuilder::Serialize(manifest).dump()).dump()<<"));if(!r.ok())throw std::runtime_error(\"E_ADAPTER_MANIFEST\");return r.manifest;}\n";
    s<<"GeneratedAdapter(){context_.threads(1);context_.randSeed(1);context_.randReset(0);model_=std::make_unique<"<<top<<">(&context_);}\n~GeneratedAdapter(){Owner();}\nvoid WriteInput(const vb::PortBitRef& ref,bool value)override{Owner();static const auto m=Manifest();if(!m.Contains(ref))throw std::runtime_error(\"E_ADAPTER_REF\");\n";
    for(const auto& p:manifest.ports)if(p.direction==Direction::Input){
        s<<"if(ref.port_id=="<<Json(p.port_id).dump()<<"){\n";
        if(p.width==1)s<<"model_->"<<p.rtl_name<<"=value;\n";
        else s<<"const uint64_t mask=UINT64_C(1)<<ref.bit_slot;model_->"<<p.rtl_name<<"=static_cast<std::remove_reference_t<decltype(model_->"<<p.rtl_name<<")>>((uint64_t(model_->"<<p.rtl_name<<")&~mask)|(value?mask:0));\n";
        s<<"return;}\n";
    }
    s<<"throw std::runtime_error(\"E_ADAPTER_REF\");}\nbool ReadOutput(const vb::PortBitRef& ref)const override{Owner();static const auto m=Manifest();if(!m.Contains(ref))throw std::runtime_error(\"E_ADAPTER_REF\");\n";
    for(const auto& p:manifest.ports)if(p.direction==Direction::Output)s<<"if(ref.port_id=="<<Json(p.port_id).dump()<<")return (uint64_t(model_->"<<p.rtl_name<<")>>ref.bit_slot)&UINT64_C(1);\n";
    s<<"throw std::runtime_error(\"E_ADAPTER_REF\");}\nvoid Evaluate(const vb::EventPosition& position)override{Owner();if(finalized_)throw std::runtime_error(\"E_MODEL_FINALIZED\");context_.time(position.tick);model_->eval();}\nvoid Finalize()override{Owner();if(finalized_)throw std::runtime_error(\"E_FINAL_TWICE\");finalized_=true;model_->final();}\n};\n";
    s<<"int main(int argc,char** argv){return vb::SessionRunnerMain(argc,argv,GeneratedAdapter::Manifest(),[]{return std::make_unique<GeneratedAdapter>();});}\n";
    r.source=s.str();r.compile_definitions={"VL_USER_FATAL"};return r;
}
}
