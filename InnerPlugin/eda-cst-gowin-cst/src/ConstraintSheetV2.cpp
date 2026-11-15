#include "eda-platform/Platform.h"
#include "ConstraintSheet.h"
#include "eda-virtual-board/PortManifestBuilder.h"
#include "eda-virtual-board/ElectricalValidator.h"
#include "Sha256.h"
#include <eda/api/Types.h>
#include <fstream>
#include <map>
#include <regex>
#include <set>
#include <sstream>

namespace eda::cst {
using namespace virtual_board;
namespace {
std::string Key(const PortBitRef& r) { return r.port_id + (r.hdl_index ? "["+std::to_string(*r.hdl_index)+"]" : "<scalar>"); }
Json RefJson(const PortBitRef& r) {
    return {{"port_id",r.port_id},{"hdl_index",r.hdl_index ? Json(*r.hdl_index) : Json(nullptr)},{"bit_slot",r.bit_slot}};
}
bool SafeText(const std::string& s) { return s.find_first_of("\r\n\";\\") == std::string::npos && s.find("//") == std::string::npos; }
bool ResolveLabel(const PortManifest& m, const std::string& label, PortBitRef& r) {
    for (const auto& p : m.ports) for (const auto& b : p.bits) if (m.Label(b) == label) { r=b; return true; }
    return false;
}
}
std::vector<Diagnostic> ConstraintValidator::Validate(const ConstraintSheetV2& s, const ElectricalProfile* e) {
    std::vector<Diagnostic> out;
    auto manifest = PortManifestBuilder::Deserialize(PortManifestBuilder::Serialize(s.manifest));
    if (!manifest.ok()) return manifest.diagnostics;
    if (s.topModule != s.manifest.top_module) out.push_back({"E_MANIFEST_TOP","Constraint top differs from manifest","top"});
    std::set<std::string> bits;
    std::set<int> pins;
    for (size_t i=0;i<s.bindings.size();++i) {
        const auto& b = s.bindings[i];
        auto error = [&](const char* c,const std::string& msg) {out.push_back({c,msg,"bindings",int(i)});};
        if (!s.manifest.Contains(b.ref)) error("E_PORT_BIT_REF","Binding is not an exact declared PortBitRef");
        if (!bits.insert(Key(b.ref)).second) error("E_DUPLICATE_BIT","Duplicate bit binding");
        if (b.package_pin <= 0) error("E_MISSING_BINDING","Unbound port bit");
        else if (!pins.insert(b.package_pin).second) error("E_DUPLICATE_PAD","Pad bound more than once");
        if (b.io_type.empty()) error("E_IO_STANDARD_UNKNOWN","Explicit IO_TYPE required");
        if (b.pull_up && b.pull_down) error("E_PULL_CONFLICT","Conflicting pull modes");
        if (!SafeText(s.manifest.Label(b.ref)) || !SafeText(b.comment) ||
            !std::regex_match(b.io_type,std::regex("[A-Za-z][A-Za-z0-9_]*")) ||
            (!b.drive.empty() && !std::regex_match(b.drive,std::regex("[0-9]+"))))
            error("E_CST_UNSAFE_TEXT","Invalid CST identifier/attribute/comment");
    }
    for (const auto& p : s.manifest.ports) for (const auto& b : p.bits)
        if (!bits.count(Key(b))) out.push_back({"E_MISSING_BINDING","Missing " + s.manifest.Label(b),"bindings"});
    if (e) {
        auto more = ValidateElectrical(*e,{s.device,s.package,s.boardRevision},s.manifest,s.bindings);
        out.insert(out.end(),more.begin(),more.end());
    }
    return out;
}
CstGenerationResult CstGenerator::Generate(const ConstraintSheetV2& s) const {
    CstGenerationResult r;
    for (const auto& d : ConstraintValidator::Validate(s)) {
        ConstraintError e; e.message = d.code+": "+d.message; e.field=d.field; e.rowIndex=d.row; r.errors.push_back(e);
    }
    if (!r.errors.empty()) return r;
    std::ostringstream body;
    // Slot order is intentional and survives storage; labels use true HDL indices.
    for (const auto& b : s.bindings) {
        auto name = s.manifest.Label(b.ref);
        body << "IO_LOC \"" << name << "\" " << b.package_pin << ";\n";
        body << "IO_PORT \"" << name << "\" IO_TYPE=" << b.io_type;
        if (!b.drive.empty()) body << " DRIVE=" << b.drive;
        body << " PULL_MODE=" << (b.pull_up ? "UP" : b.pull_down ? "DOWN" : "NONE") << ";\n";
        if (!b.comment.empty()) body << "// " << b.comment << "\n";
    }
    auto text = body.str(); r.bindingsHash=platform::Sha256Hex(text.data(),text.size());
    r.cstContent = "// SigFlow CST V2; manifest source " + s.manifest.source_sha256 + "\n// bindings-sha256 " + r.bindingsHash + "\n" + text;
    r.success=true; return r;
}
CstImportV2 CstGenerator::ImportCst(const std::string& text,const PortManifest& m) const {
    CstImportV2 r;
    auto validated=PortManifestBuilder::Deserialize(PortManifestBuilder::Serialize(m));
    if(!validated.ok()){r.diagnostics=validated.diagnostics;return r;}
    std::map<std::string,PinBindingV2> table;
    std::set<std::string> locs, attrs;
    const std::regex loc(R"rx(^\s*IO_LOC\s+"([^"]+)"\s+([0-9]+)\s*;\s*$)rx");
    const std::regex attr(R"rx(^\s*IO_PORT\s+"([^"]+)"\s+([^;]+)\s*;\s*$)rx");
    const std::regex token(R"(^([A-Z_]+)=([A-Za-z0-9_]+)$)");
    std::istringstream lines(text); std::string line; int number=0;
    while (std::getline(lines,line)) {
        ++number;
        auto comment=line.find("//"); if (comment!=std::string::npos) line.erase(comment);
        if (line.find_first_not_of(" \t\r\n")==std::string::npos) continue;
        std::smatch match;
        bool isloc=std::regex_match(line,match,loc);
        if (!isloc && !std::regex_match(line,match,attr)) {
            r.unmanagedLines.push_back(line); r.diagnostics.push_back({"E_CST_UNMANAGED","Unmanaged CST statement", "line",number}); continue;
        }
        std::string name=match[1]; PortBitRef ref;
        if (!ResolveLabel(m,name,ref)) {r.diagnostics.push_back({"E_PORT_BIT_REF","Unknown or incorrectly indexed port: "+name,"line",number});continue;}
        auto& b=table[name]; b.ref=ref; b.source="import";
        if (isloc) {
            if (!locs.insert(name).second) r.diagnostics.push_back({"E_DUPLICATE_BIT","Duplicate IO_LOC","line",number});
            try { b.package_pin=std::stoi(match[2]); } catch (...) {r.diagnostics.push_back({"E_PAD_INVALID","Invalid pad","line",number});}
        } else {
            if (!attrs.insert(name).second) r.diagnostics.push_back({"E_CST_DUPLICATE_ATTRIBUTE","Duplicate IO_PORT","line",number});
            std::istringstream tokens(match[2]); std::string t; std::set<std::string> seen;
            while(tokens>>t) {
                std::smatch pair;
                if (!std::regex_match(t,pair,token) || !seen.insert(pair[1]).second) {
                    r.diagnostics.push_back({"E_CST_UNMANAGED","Unknown or duplicate attribute: "+t,"line",number}); continue;
                }
                std::string key=pair[1],v=pair[2];
                if(key=="IO_TYPE") b.io_type=v;
                else if(key=="DRIVE" && std::regex_match(v,std::regex("[0-9]+"))) b.drive=v;
                else if(key=="PULL_MODE" && (v=="UP" || v=="DOWN" || v=="NONE")) {b.pull_up=v=="UP";b.pull_down=v=="DOWN";}
                else {r.unmanagedLines.push_back(t);r.diagnostics.push_back({"E_CST_UNMANAGED","Unmanaged electrical attribute: "+t,"line",number});}
            }
        }
    }
    for (auto& [name,b]:table) {
        if (!locs.count(name) || b.io_type.empty()) r.diagnostics.push_back({"E_CST_INCOMPLETE","IO_LOC and explicit IO_TYPE required: "+name,"cst"});
        r.bindings.push_back(b);
    }
    return r;
}
bool SaveConstraintSheet(const std::filesystem::path& path,const ConstraintSheetV2& s,std::vector<Diagnostic>& errors) {
    // Drafts may contain unbound rows; saving must still preserve every ref losslessly.
    try {
        auto valid=PortManifestBuilder::Deserialize(PortManifestBuilder::Serialize(s.manifest));
        if(!valid.ok()){errors=valid.diagnostics;return false;}
        for (const auto& b : s.bindings) if (!s.manifest.Contains(b.ref)) {
            errors.push_back({"E_PORT_BIT_REF","Cannot save a reference outside the current declaration","bindings"});
            return false;
        }
        Json j={{"schema","sigflow.pin-bindings.v2"},{"manifest",PortManifestBuilder::Serialize(s.manifest)},
            {"target_profile_id",s.targetProfileId},{"target_profile_version",s.targetProfileVersion},
            {"top_module",s.topModule},{"device",s.device},{"package",s.package},
            {"board_revision",s.boardRevision},{"generated_at",s.generatedAt},{"bindings",Json::array()}};
        for(const auto& b:s.bindings) j["bindings"].push_back({{"ref",RefJson(b.ref)}, {"package_pin",b.package_pin},
            {"io_type",b.io_type},{"drive",b.drive},{"pull_up",b.pull_up},{"pull_down",b.pull_down},
            {"comment",b.comment},{"source",b.source}});
        std::ofstream f(path,std::ios::binary); f << j.dump(2) << '\n';
        if(!f)throw std::runtime_error("Cannot write bindings file"); return true;
    }catch(const std::exception& e){errors.push_back({"E_BINDINGS_IO",e.what(),eda::platform::PathToUtf8(path)});return false;}
}
bool LoadConstraintSheet(const std::filesystem::path& path,const PortManifest& current,
                         ConstraintSheetV2& result,std::vector<Diagnostic>& errors) {
    try {
        std::ifstream f(path);Json j;f>>j; ConstraintSheetV2 s; s.manifest=current;
        s.topModule=j.value("top_module",current.top_module);s.device=j.value("device","");
        s.targetProfileId=j.value("target_profile_id",j.value("target_profile",""));s.targetProfileVersion=j.value("target_profile_version",j.value("profile_version",""));
        s.package=j.value("package","QFN88");s.boardRevision=j.value("board_revision","6202");s.generatedAt=j.value("generated_at","");
        bool v2=j.value("schema","")=="sigflow.pin-bindings.v2";
        if (j.contains("schema") && !v2) throw std::runtime_error("E_MANIFEST_VERSION");
        if (!v2 && j.value("schema_version","")!="1.0") throw std::runtime_error("E_MANIFEST_VERSION");
        if (v2) {
            auto stored=PortManifestBuilder::Deserialize(j.at("manifest"));
            if(!stored.ok()){errors=stored.diagnostics;return false;}
            if(PortManifestBuilder::Serialize(stored.manifest)!=PortManifestBuilder::Serialize(current)) {
                errors.push_back({"E_MANIFEST_STALE","Stored and current declaration/source snapshots differ","manifest"});return false;
            }
        }
        for(const auto& v:j.at("bindings")) {
            PinBindingV2 b;
            if(v2) {
                auto ref=v.at("ref");b.ref.port_id=ref.at("port_id");b.ref.bit_slot=ref.at("bit_slot");
                if(!ref.at("hdl_index").is_null()) b.ref.hdl_index=ref.at("hdl_index").get<int64_t>();
            }else{
                std::string name=v.at("port"); int64_t index=v.value("bit",int64_t(0));
                const PortDecl* p=nullptr;for(const auto& port:current.ports)if(port.rtl_name==name)p=&port;
                // Legacy vector ordinals and HDL indices were conflated. Only [N:0]
                // with N>0 is uniquely recoverable. Scalar 0 is known by declaration.
                if(!p || (p->shape==PortShape::PackedVector &&
                   (p->width==1 || *p->decl_right!=0 || *p->decl_left<=0))) {
                    errors.push_back({"E_LEGACY_BINDING_AMBIGUOUS","Manual migration required: "+name,"bindings"});continue;
                }
                bool found=false;
                for(const auto& ref:p->bits) if((p->shape==PortShape::Scalar && index==0) || ref.hdl_index==index) {b.ref=ref;found=true;break;}
                if(!found){errors.push_back({"E_PORT_BIT_REF","Legacy index outside declaration","bindings"});continue;}
            }
            if(!current.Contains(b.ref)){errors.push_back({"E_PORT_BIT_REF","Stored ref invalid","bindings"});continue;}
            b.package_pin=v.value("package_pin",-1);b.io_type=v.value("io_type","");b.drive=v.value("drive","");
            b.pull_up=v.value("pull_up",false);b.pull_down=v.value("pull_down",false);b.comment=v.value("comment","");b.source=v.value("source","");
            s.bindings.push_back(b);
        }
        if(!errors.empty())return false;result=std::move(s);return true;
    }catch(const std::exception& e){errors.push_back({"E_BINDINGS_LOAD",e.what(),eda::platform::PathToUtf8(path)});return false;}
}
}
