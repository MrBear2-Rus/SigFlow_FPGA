#include "eda-platform/Platform.h"
#include "FpgaConstraint.h"
#include "eda-virtual-board/PortManifestBuilder.h"
#include "eda-virtual-board/ElectricalValidator.h"
#include "eda-platform/Sha256.h"
#include "platform/PlatformPaths.h"
#include <fstream>
using namespace eda::virtual_board;
namespace {
wxString W(const std::string& s){return wxString::FromUTF8(s.c_str());}
std::string S(const wxString& s){return sigflow::platform::Utf8String(s);}
std::vector<ConstraintError> Errors(const std::vector<Diagnostic>& list){
    std::vector<ConstraintError> out;
    for(const auto& d:list){ConstraintError e;e.message=W(d.code+": "+d.message);e.field=W(d.field);e.rowIndex=d.row;out.push_back(e);}return out;
}
wxString ErrorText(const std::vector<Diagnostic>& ds){wxString s;for(const auto& d:ds)s+=W(d.code+": "+d.message)+"\n";return s;}
PinBindingV2 ToBinding(const PinBinding& b){
    PinBindingV2 r;r.ref=b.ref;r.package_pin=b.packagePin;r.io_type=S(b.ioType);r.drive=S(b.drive);
    r.pull_up=b.pullUp;r.pull_down=b.pullDown;r.comment=S(b.comment);r.source=S(b.source);return r;
}
}
eda::cst::ConstraintSheetV2 ToCoreConstraintSheet(const ConstraintSheet& s){
    eda::cst::ConstraintSheetV2 r;r.manifest=s.manifest;r.targetProfileId=S(s.targetProfileId);
    r.targetProfileVersion=S(s.targetProfileVersion);r.topModule=S(s.topModule);r.device=S(s.device);
    r.package=S(s.package);r.boardRevision=S(s.boardRevision);r.generatedAt=S(s.generatedAt);
    for(const auto& b:s.bindings)r.bindings.push_back(ToBinding(b));return r;
}
ConstraintSheet FromCoreConstraintSheet(const eda::cst::ConstraintSheetV2& s){
    ConstraintSheet r;r.manifest=s.manifest;r.topModule=W(s.topModule);r.device=W(s.device);
    r.targetProfileId=W(s.targetProfileId);r.targetProfileVersion=W(s.targetProfileVersion);
    r.package=W(s.package);r.boardRevision=W(s.boardRevision);r.generatedAt=W(s.generatedAt);
    for(const auto& b:s.bindings){
        PinBinding v;v.ref=b.ref;v.portName=W(s.manifest.Label(b.ref));
        const auto* p=s.manifest.Find(b.ref.port_id);if(p)v.direction=static_cast<FpgaPortDirection>(p->direction);
        v.packagePin=b.package_pin;v.ioType=W(b.io_type);v.drive=W(b.drive);v.pullUp=b.pull_up;v.pullDown=b.pull_down;
        v.comment=W(b.comment);v.source=W(b.source);r.bindings.push_back(v);
    }return r;
}
std::vector<PinBinding> MakeGuiBindingRows(const FpgaPortMap& m){
    eda::cst::ConstraintSheetV2 s;s.manifest=m;
    // HDL declaration display order is separate from persistent bit_slot order.
    for(const auto& p:m.ports)for(auto it=p.bits.rbegin();it!=p.bits.rend();++it){PinBindingV2 b;b.ref=*it;b.source="gui";s.bindings.push_back(b);}
    return FromCoreConstraintSheet(s).bindings;
}
PinBinding* ConstraintSheet::FindBinding(const wxString& label){for(auto& b:bindings)if(b.portName==label)return &b;return nullptr;}
const PinBinding* ConstraintSheet::FindBinding(const wxString& label)const{for(const auto& b:bindings)if(b.portName==label)return &b;return nullptr;}
FpgaPortMap ConstraintValidator::ExtractPortsFromYosysJson(const wxString& path,const wxString& top,wxString& error)const{
    error.clear();try{
        auto file=sigflow::platform::Utf8Path(path);std::ifstream f(sigflow::platform::Utf8Path(path+".ports.v2.json"));eda::Json j;f>>j;
        if(j.at("yosys_sha256").get<std::string>()!=eda::platform::Sha256FileHex(file))throw std::runtime_error("E_MANIFEST_STALE: Yosys SHA256 mismatch");
        auto result=PortManifestBuilder::Deserialize(j.at("manifest"));if(!result.ok()){error=ErrorText(result.diagnostics);return {};}
        if(j.at("schema")!="sigflow.port-export.v2")throw std::runtime_error("E_MANIFEST_VERSION");
        if(result.manifest.top_module!=S(top))throw std::runtime_error("E_MANIFEST_TOP");
        if(j.at("sources").empty())throw std::runtime_error("E_MANIFEST_IDENTITY: no source snapshot");
        if(PortManifestBuilder::SnapshotFingerprint(j)!=result.manifest.source_sha256)
            throw std::runtime_error("E_MANIFEST_IDENTITY: source fingerprint mismatch");
        for(const auto& source:j.at("sources")){
            auto sourcePath=eda::platform::PathFromUtf8(source.at("path").get<std::string>());
            if(eda::platform::Sha256FileHex(sourcePath)!=source.at("sha256").get<std::string>())throw std::runtime_error("E_MANIFEST_STALE: HDL source changed");
        }
        for(const auto& source:j.at("dependencies")){
            auto sourcePath=eda::platform::PathFromUtf8(source.at("path").get<std::string>());
            if(eda::platform::Sha256FileHex(sourcePath)!=source.at("sha256").get<std::string>())throw std::runtime_error("E_MANIFEST_STALE: dependency changed");
        }
        return result.manifest;
    }catch(const std::exception& e){error=W(std::string("E_MANIFEST_REQUIRED: Export dual-source ports with tools/virtual/export_ports.py. ")+e.what());return {};}
}
std::vector<ConstraintError> ConstraintValidator::Validate(const ConstraintSheet& s,const FpgaPortMap& ports)const{
    auto sheet=ToCoreConstraintSheet(s);
    if(PortManifestBuilder::Serialize(ports)!=PortManifestBuilder::Serialize(sheet.manifest))return Errors({{"E_MANIFEST_STALE","GUI and constraint manifests differ","manifest"}});
    ElectricalProfile profile;std::vector<Diagnostic> ds;
    if(!LoadElectricalProfile(sigflow::platform::Utf8Path(m_pinDb.electricalProfilePath),profile,ds))return Errors(ds);
    return Errors(eda::cst::ConstraintValidator::Validate(sheet,&profile));
}
std::vector<ConstraintError> ConstraintValidator::ValidateBinding(const PinBinding& b,const FpgaPortMap& m)const{
    ElectricalProfile p;std::vector<Diagnostic> ds;
    if(!LoadElectricalProfile(sigflow::platform::Utf8Path(m_pinDb.electricalProfilePath),p,ds))return Errors(ds);
    return Errors(ValidateElectrical(p,{S(m_pinDb.device),S(m_pinDb.packageName),S(m_pinDb.boardRevision)},m,{ToBinding(b)}));
}
CstGenerationResult CstGenerator::Generate(const ConstraintSheet& s)const{
    auto r=eda::cst::CstGenerator().Generate(ToCoreConstraintSheet(s));CstGenerationResult out;
    out.success=r.success;out.cstContent=W(r.cstContent);out.bindingsHash=W(r.bindingsHash);
    for(const auto& e:r.errors){ConstraintError d;d.message=W(e.message);d.field=W(e.field);d.rowIndex=e.rowIndex;out.errors.push_back(d);}return out;
}
std::vector<PinBinding> CstGenerator::ImportCst(const wxString& text,const FpgaPortMap& m,std::vector<wxString>& unmanaged,wxString& error)const{
    auto r=eda::cst::CstGenerator().ImportCst(S(text),m);unmanaged.clear();error=ErrorText(r.diagnostics);
    for(const auto& line:r.unmanagedLines)unmanaged.push_back(W(line));
    if(!r.ok())return {};eda::cst::ConstraintSheetV2 sheet;sheet.manifest=m;sheet.bindings=r.bindings;return FromCoreConstraintSheet(sheet).bindings;
}
bool LoadConstraintSheet(const wxString& path,ConstraintSheet& sheet,wxString& error){
    std::vector<Diagnostic> ds;auto current=sheet.manifest;
    // Read-only TraceBridge may inspect an embedded V2 manifest; legacy
    // migration always needs an external current declaration.
    if(current.empty())try{std::ifstream f(sigflow::platform::Utf8Path(path));eda::Json j;f>>j;
        auto r=PortManifestBuilder::Deserialize(j.at("manifest"));if(!r.ok()){error=ErrorText(r.diagnostics);return false;}current=r.manifest;
    }catch(const std::exception& e){error=W(std::string("E_MANIFEST_REQUIRED: ")+e.what());return false;}
    eda::cst::ConstraintSheetV2 r;if(!eda::cst::LoadConstraintSheet(sigflow::platform::Utf8Path(path),current,r,ds)){error=ErrorText(ds);return false;}
    sheet=FromCoreConstraintSheet(r);error.clear();return true;
}
bool SaveConstraintSheet(const wxString& path,const ConstraintSheet& sheet,wxString& error){
    std::vector<Diagnostic> ds;std::error_code ec;std::filesystem::create_directories(sigflow::platform::Utf8Path(path).parent_path(),ec);
    bool ok=eda::cst::SaveConstraintSheet(sigflow::platform::Utf8Path(path),ToCoreConstraintSheet(sheet),ds);error=ErrorText(ds);return ok;
}
wxString FindYosysJsonPath(const wxString& root,const wxString& top){
    auto declarations=sigflow::platform::Utf8Path(root)/".sigflow"/"virtual"/"declarations"/
        eda::platform::PathFromUtf8(S(top)+".json");
    if(std::filesystem::is_regular_file(declarations))return W(eda::platform::PathToUtf8(declarations));
    return W(sigflow::platform::PathToUtf8(eda::cst::FindYosysJsonPath(sigflow::platform::Utf8Path(root),S(top))));
}
