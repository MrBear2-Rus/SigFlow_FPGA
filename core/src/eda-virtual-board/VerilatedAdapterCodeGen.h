#pragma once
#include <eda/api/port_manifest.hpp>
namespace eda::virtual_board {
struct AdapterGeneration {std::string source;std::vector<std::string> compile_definitions;std::vector<Diagnostic> diagnostics;bool ok()const{return diagnostics.empty()&&!source.empty();}};
class VerilatedAdapterCodeGen {
public:
    static AdapterGeneration Generate(const PortManifest&,const std::string& top_class);
};
}
