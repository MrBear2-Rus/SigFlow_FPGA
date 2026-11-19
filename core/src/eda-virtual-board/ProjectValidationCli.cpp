#include "ProjectValidation.h"
#include "PortManifestBuilder.h"
#include "eda-platform/Platform.h"
#include <fstream>
#include <iostream>
int main(int argc,char** argv){try{
    if(argc!=3)throw std::runtime_error("usage: sigflow_session_validate ports.v2.json session.json");
    eda::Json exported,config;std::ifstream(eda::platform::PathFromUtf8(argv[1]))>>exported;std::ifstream(eda::platform::PathFromUtf8(argv[2]))>>config;
    auto m=eda::virtual_board::PortManifestBuilder::Deserialize(exported.at("manifest"));if(!m.ok())throw std::runtime_error("E_PROJECT_MANIFEST");
    auto result=eda::virtual_board::ValidateVirtualProject(config,m.manifest);std::cout<<result.dump()<<std::endl;return result.at("ok").get<bool>()?0:2;
}catch(const std::exception& e){std::cout<<eda::Json{{"ok",false},{"diagnostics",eda::Json::array({{{"code","E_PROJECT_CONFIGURATION"},{"message",e.what()},{"suggestion","核对实验描述和工具链。"}}})}}.dump()<<std::endl;return 2;}}
