#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace eda::virtual_board {
enum class PortShape { Scalar, PackedVector };
enum class Direction { Input, Output, InOut };
inline std::string DirectionName(Direction d) {
    return d == Direction::Input ? "input" : d == Direction::Output ? "output" : "inout";
}
struct Diagnostic {
    std::string code, message, field;
    int row = -1;
    std::string port_ref, vendor_bank_id, requested_io_type;
    std::optional<int> package_pin, vccio_mv;
    std::vector<std::string> provenance_refs;
    std::string remediation;
};
struct PortBitRef {
    std::string port_id;
    std::optional<int64_t> hdl_index;
    uint32_t bit_slot = 0;
    bool operator==(const PortBitRef& b) const {
        return port_id == b.port_id && hdl_index == b.hdl_index && bit_slot == b.bit_slot;
    }
};
struct PortDecl {
    std::string port_id, rtl_name;
    Direction direction = Direction::Input;
    PortShape shape = PortShape::Scalar;
    std::optional<int64_t> decl_left, decl_right;
    uint32_t width = 1;
    std::string source_ref;
    std::vector<PortBitRef> bits; // storage order, NOT HDL display order
};
struct PortManifest {
    std::string schema = "sigflow.port-manifest.v2";
    std::string top_module, source_sha256;
    std::vector<PortDecl> ports;
    bool empty() const { return ports.empty(); }
    void clear() { ports.clear(); }
    const PortDecl* Find(const std::string& id) const {
        for (const auto& p : ports) if (p.port_id == id) return &p;
        return nullptr;
    }
    bool Contains(const PortBitRef& r) const {
        auto p = Find(r.port_id);
        if (p) for (const auto& b : p->bits) if (b == r) return true;
        return false;
    }
    std::string Label(const PortBitRef& r) const {
        auto p = Find(r.port_id);
        if (!p) return {};
        return p->rtl_name + (r.hdl_index ? "[" + std::to_string(*r.hdl_index) + "]" : "");
    }
};
struct PinBindingV2 {
    PortBitRef ref;
    int package_pin = -1;
    std::string io_type; // deliberately no global electrical default
    std::string drive;
    bool pull_up = false, pull_down = false;
    std::string comment, source;
};
inline constexpr const char* E_PORT_SHAPE_UNKNOWN = "E_PORT_SHAPE_UNKNOWN";
inline constexpr const char* E_MANIFEST_VERSION = "E_MANIFEST_VERSION";
struct ManifestResult {
    PortManifest manifest;
    std::vector<Diagnostic> diagnostics;
    bool ok() const { return diagnostics.empty() && !manifest.empty(); }
};
} // namespace eda::virtual_board
