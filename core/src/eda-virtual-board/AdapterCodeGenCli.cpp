#include "VerilatedAdapterCodeGen.h"
#include "PortManifestBuilder.h"
#include "eda-platform/Platform.h"
#include <fstream>
#include <iostream>
int main(int argc,char** argv){
    try{
        if(argc!=4)throw std::runtime_error("usage: sigflow_adapter_codegen port-export.json Vtop output.cpp");
        std::ifstream f(eda::platform::PathFromUtf8(argv[1]));eda::Json j;f>>j;
        auto manifest=eda::virtual_board::PortManifestBuilder::Deserialize(j.at("manifest"));if(!manifest.ok())throw std::runtime_error("E_ADAPTER_MANIFEST");
        auto generated=eda::virtual_board::VerilatedAdapterCodeGen::Generate(manifest.manifest,argv[2]);
        if(!generated.ok()){for(const auto& d:generated.diagnostics)std::cerr<<d.code<<": "<<d.message<<'\n';return 1;}
        std::ofstream out(eda::platform::PathFromUtf8(argv[3]),std::ios::binary);out<<generated.source;if(!out)throw std::runtime_error("E_ADAPTER_WRITE");
        std::cout<<eda::Json{{"compile_definitions",generated.compile_definitions},{"schema","sigflow.verilated-adapter.v1"}}.dump()<<'\n';return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
