#pragma once
#include <eda/api/port_manifest.hpp>
#include <eda/api/Types.h>
#include <filesystem>
namespace eda::virtual_board {
class PortManifestBuilder {
public:
    // Both exports must belong to this source snapshot/top/parameterization.
    static ManifestResult Build(const Json& verilator_ast, const Json& yosys,
                                const std::string& top, const std::string& source_sha256);
    static Json Serialize(const PortManifest&);
    static ManifestResult Deserialize(const Json&);
    static ManifestResult Load(const std::filesystem::path&);
    static std::string SnapshotFingerprint(const Json& export_snapshot);
};
}
