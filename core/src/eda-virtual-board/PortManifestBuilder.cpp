#include "eda-platform/Platform.h"
#include "PortManifestBuilder.h"
#include "eda-platform/Sha256.h"
#include <algorithm>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>

namespace eda::virtual_board {
namespace {
Direction ParseDirection(std::string d) {
    if (d == "INPUT" || d == "input") return Direction::Input;
    if (d == "OUTPUT" || d == "output") return Direction::Output;
    if (d == "INOUT" || d == "inout") return Direction::InOut;
    throw std::runtime_error("E_PORT_DIRECTION: " + d);
}
int64_t Integer(const std::string& s) {
    size_t n = 0;
    auto v = std::stoll(s, &n);
    if (n != s.size()) throw std::runtime_error("E_PORT_RANGE: " + s);
    return v;
}
void Check(PortManifest& m) {
    if (m.schema != "sigflow.port-manifest.v2") throw std::runtime_error(E_MANIFEST_VERSION);
    if (m.top_module.empty() || m.source_sha256.size() != 64 || m.ports.empty())
        throw std::runtime_error("E_MANIFEST_IDENTITY");
    std::set<std::string> ids, names;
    for (const auto& p : m.ports) {
        if (p.port_id != m.top_module + "::" + p.rtl_name || !ids.insert(p.port_id).second ||
            !names.insert(p.rtl_name).second || p.rtl_name.empty()) throw std::runtime_error("E_PORT_ID");
        if (!p.width || p.bits.size() != p.width || p.width > 1048576) throw std::runtime_error("E_PORT_WIDTH");
        if (p.shape == PortShape::Scalar) {
            if (p.width != 1 || p.decl_left || p.decl_right || p.bits[0].hdl_index)
                throw std::runtime_error("E_PORT_SHAPE_CONFLICT");
        } else if (!p.decl_left || !p.decl_right ||
                   static_cast<uint64_t>(std::max(*p.decl_left,*p.decl_right)) -
                   static_cast<uint64_t>(std::min(*p.decl_left,*p.decl_right)) + 1 != p.width)
            throw std::runtime_error("E_PORT_RANGE");
        for (uint32_t i = 0; i < p.width; ++i) {
            auto expected = p.shape == PortShape::Scalar ? std::optional<int64_t>{} :
                std::optional<int64_t>{*p.decl_right + (*p.decl_left < *p.decl_right ? -int64_t(i) : int64_t(i))};
            if (p.bits[i].port_id != p.port_id || p.bits[i].bit_slot != i || p.bits[i].hdl_index != expected)
                throw std::runtime_error("E_PORT_BIT_REF");
        }
    }
}
}
ManifestResult PortManifestBuilder::Build(const Json& ast, const Json& y, const std::string& top,
                                          const std::string& hash) {
    ManifestResult r;
    r.manifest.top_module = top; r.manifest.source_sha256 = hash;
    try {
        std::map<std::string, Json> types;
        for (const auto& n : ast.at("miscsp")) if (n.value("type", "") == "TYPETABLE")
            for (const auto& t : n.at("typesp")) types[t.at("addr").get<std::string>()] = t;
        const Json* module = nullptr;
        for (const auto& n : ast.at("modulesp")) if (n.value("name", "") == top) {
            if (module) throw std::runtime_error("E_MANIFEST_TOP");
            module = &n;
        }
        if (!module) throw std::runtime_error("E_MANIFEST_TOP");
        const auto& ym = y.at("modules").at(top);
        const auto& yp = ym.at("ports");
        for (const auto& n : module->at("stmtsp")) {
            if (n.value("type", "") != "VAR" || !n.value("isPrimaryIO", false)) continue;
            PortDecl p;
            p.rtl_name = n.at("name"); p.port_id = top + "::" + p.rtl_name;
            p.direction = ParseDirection(n.at("direction")); p.source_ref = n.value("loc", "");
            const auto& t = types.at(n.at("dtypep").get<std::string>());
            if (t.value("type", "") != "BASICDTYPE") throw std::runtime_error(E_PORT_SHAPE_UNKNOWN);
            if (t.contains("range") && !t["range"].is_null()) {
                std::string range = t.at("range");
                auto sep = range.find(':');
                if (sep == std::string::npos) throw std::runtime_error(E_PORT_SHAPE_UNKNOWN);
                p.decl_left = Integer(range.substr(0, sep)); p.decl_right = Integer(range.substr(sep+1));
                p.shape = PortShape::PackedVector;
                uint64_t delta = static_cast<uint64_t>(std::max(*p.decl_left,*p.decl_right)) -
                                 static_cast<uint64_t>(std::min(*p.decl_left,*p.decl_right));
                if (delta >= 1048576) throw std::runtime_error("E_PORT_WIDTH");
                p.width = uint32_t(delta + 1);
            }
            const auto& q = yp.at(p.rtl_name);
            if (ParseDirection(q.at("direction")) != p.direction || q.at("bits").size() != p.width)
                throw std::runtime_error("E_PORT_SOURCE_CONFLICT");
            int64_t offset = q.value("offset", int64_t(0));
            int upto = q.value("upto", 0);
            bool single = false;
            if (ym.contains("netnames") && ym["netnames"].contains(p.rtl_name)) {
                const auto& attrs = ym["netnames"][p.rtl_name].value("attributes", Json::object());
                if (attrs.contains("single_bit_vector")) {
                    const auto& v = attrs["single_bit_vector"];
                    if(v.is_string()){
                        auto flag=v.get<std::string>();
                        if(flag.empty() || flag.find_first_not_of("01")!=std::string::npos ||
                           (flag.find('1')!=std::string::npos && flag.find('1')!=flag.size()-1))
                            throw std::runtime_error("E_PORT_SOURCE_CONFLICT");
                        single=flag.back()=='1';
                    }else if(v.is_number_integer() && (v.get<int>()==0 || v.get<int>()==1))single=v.get<int>()==1;
                    else throw std::runtime_error("E_PORT_SOURCE_CONFLICT");
                }
            }
            if(single && p.width!=1)throw std::runtime_error("E_PORT_SOURCE_CONFLICT");
            if (upto != 0 && upto != 1) throw std::runtime_error("E_PORT_SOURCE_CONFLICT");
            if (p.shape == PortShape::Scalar) {
                if (single || offset || upto) throw std::runtime_error("E_PORT_SHAPE_CONFLICT");
            } else {
                if (offset != std::min(*p.decl_left,*p.decl_right) ||
                    upto != int(*p.decl_left < *p.decl_right) || (p.width == 1 && !single))
                    throw std::runtime_error("E_PORT_SOURCE_CONFLICT");
            }
            for (uint32_t i=0; i<p.width; ++i) p.bits.push_back({p.port_id,
                p.shape == PortShape::Scalar ? std::optional<int64_t>{} :
                std::optional<int64_t>{offset + (upto ? int64_t(p.width)-1-i : int64_t(i))}, i});
            r.manifest.ports.push_back(std::move(p));
        }
        if (r.manifest.ports.size() != yp.size()) throw std::runtime_error("E_PORT_SOURCE_CONFLICT");
        Check(r.manifest);
    } catch (const std::exception& e) {
        std::string message = e.what();
        auto end = message.find(':');
        auto code = message.substr(0,end);
        if (code.rfind("E_",0) != 0) code = "E_PORT_SHAPE_UNKNOWN";
        r.diagnostics.push_back({code,message,"manifest"}); r.manifest.ports.clear();
    }
    return r;
}
Json PortManifestBuilder::Serialize(const PortManifest& m) {
    Json j = {{"schema",m.schema},{"top_module",m.top_module},{"source_sha256",m.source_sha256},{"ports",Json::array()}};
    for (const auto& p : m.ports) {
        Json v = {{"port_id",p.port_id},{"rtl_name",p.rtl_name},{"direction",DirectionName(p.direction)},
            {"shape",p.shape == PortShape::Scalar ? "scalar" : "packed_vector"},{"width",p.width},
            {"decl_left",p.decl_left ? Json(*p.decl_left) : Json(nullptr)},
            {"decl_right",p.decl_right ? Json(*p.decl_right) : Json(nullptr)}, {"source_ref",p.source_ref},{"bits",Json::array()}};
        for (const auto& b : p.bits) v["bits"].push_back({{"port_id",b.port_id},
            {"hdl_index",b.hdl_index ? Json(*b.hdl_index) : Json(nullptr)}, {"bit_slot",b.bit_slot}});
        j["ports"].push_back(v);
    }
    return j;
}
ManifestResult PortManifestBuilder::Deserialize(const Json& j) {
    ManifestResult r;
    try {
        r.manifest.schema = j.at("schema");
        if (r.manifest.schema != "sigflow.port-manifest.v2") throw std::runtime_error(E_MANIFEST_VERSION);
        r.manifest.top_module = j.at("top_module"); r.manifest.source_sha256 = j.at("source_sha256");
        for (const auto& v : j.at("ports")) {
            PortDecl p; p.port_id = v.at("port_id"); p.rtl_name = v.at("rtl_name");
            p.direction = ParseDirection(v.at("direction")); p.width = v.at("width"); p.source_ref = v.at("source_ref");
            auto shape = v.at("shape").get<std::string>();
            if (shape != "scalar" && shape != "packed_vector") throw std::runtime_error(E_PORT_SHAPE_UNKNOWN);
            p.shape = shape == "scalar" ? PortShape::Scalar : PortShape::PackedVector;
            if (!v.at("decl_left").is_null()) p.decl_left = v.at("decl_left").get<int64_t>();
            if (!v.at("decl_right").is_null()) p.decl_right = v.at("decl_right").get<int64_t>();
            for (const auto& b : v.at("bits")) {
                PortBitRef ref; ref.port_id = b.at("port_id"); ref.bit_slot = b.at("bit_slot");
                if (!b.at("hdl_index").is_null()) ref.hdl_index = b.at("hdl_index").get<int64_t>();
                p.bits.push_back(ref);
            }
            r.manifest.ports.push_back(p);
        }
        Check(r.manifest);
    } catch(const std::exception& e) {
        std::string s = e.what(); r.diagnostics.push_back({s.rfind("E_",0)==0?s:"E_MANIFEST_INVALID",s,"manifest"});
        r.manifest.ports.clear();
    }
    return r;
}
ManifestResult PortManifestBuilder::Load(const std::filesystem::path& p) {
    try { std::ifstream f(p); Json j; f >> j; return Deserialize(j); }
    catch(const std::exception& e) { return {{},{{"E_MANIFEST_INVALID",e.what(),eda::platform::PathToUtf8(p)}}}; }
}
std::string PortManifestBuilder::SnapshotFingerprint(const Json& j) {
    Json stable={{"sources",j.at("sources")},{"dependencies",j.at("dependencies")},
        {"tools",j.at("tools")},{"parameters",j.at("parameters")},{"adapter",j.at("adapter")}};
    auto text=stable.dump();return platform::Sha256Hex(text.data(),text.size());
}
}
