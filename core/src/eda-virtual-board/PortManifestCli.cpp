#include "eda-platform/Platform.h"
#include "PortManifestBuilder.h"
#include "eda-platform/Sha256.h"
#include <fstream>
#include <iostream>
namespace fs=std::filesystem;
int main(int argc,char** argv){
    if(argc!=6){std::cerr<<"usage: sigflow_port_manifest AST YOSYS TOP SNAPSHOT OUTPUT\n";return 2;}
    try{
        auto read=[](const char* p){std::ifstream f(eda::platform::PathFromUtf8(p));eda::Json j;f>>j;return j;};
        auto ast=read(argv[1]),y=read(argv[2]),snapshot=read(argv[4]);
        if(snapshot.at("sources").empty())throw std::runtime_error("E_MANIFEST_IDENTITY");
        for(const auto& s:snapshot.at("sources"))
            if(eda::platform::Sha256FileHex(eda::platform::PathFromUtf8(s.at("path").get<std::string>()))!=s.at("sha256").get<std::string>())
                throw std::runtime_error("E_MANIFEST_STALE");
        for(const auto& s:snapshot.at("dependencies"))
            if(eda::platform::Sha256FileHex(eda::platform::PathFromUtf8(s.at("path").get<std::string>()))!=s.at("sha256").get<std::string>())
                throw std::runtime_error("E_MANIFEST_STALE: dependency changed");
        if(eda::platform::Sha256FileHex(eda::platform::PathFromUtf8(argv[1]))!=snapshot.at("ast_sha256").get<std::string>() ||
           eda::platform::Sha256FileHex(eda::platform::PathFromUtf8(argv[2]))!=snapshot.at("yosys_sha256").get<std::string>())
            throw std::runtime_error("E_EXPORT_SNAPSHOT_CONFLICT");
        auto hash=eda::virtual_board::PortManifestBuilder::SnapshotFingerprint(snapshot);
        auto r=eda::virtual_board::PortManifestBuilder::Build(ast,y,argv[3],hash);
        if(!r.ok()){
            for(const auto& d:r.diagnostics)std::cerr<<d.code<<": "<<d.message<<'\n';return 1;
        }
        snapshot["schema"]="sigflow.port-export.v2";
        snapshot["manifest"]=eda::virtual_board::PortManifestBuilder::Serialize(r.manifest);
        std::ofstream out(eda::platform::PathFromUtf8(argv[5]),std::ios::binary);out<<snapshot.dump(2)<<'\n';
        if(!out)throw std::runtime_error("E_MANIFEST_WRITE");
        std::cout<<r.manifest.ports.size()<<" cross-checked ports\n";return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
