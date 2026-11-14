#include "fpga/FpgaConstraint.h"
#include "eda-virtual-board/PortManifestBuilder.h"
#include "eda-virtual-board/ElectricalValidator.h"
#include "eda-platform/Platform.h"
#include "eda-platform/Sha256.h"
#include <algorithm>
#include <fstream>
#include <iostream>
#include <stdexcept>
using eda::Json;
namespace vb=eda::virtual_board;
namespace fs=std::filesystem;
namespace {
int checks=0;
void Check(bool b,const std::string& text){++checks;if(!b)throw std::runtime_error(text);}
Json Read(const fs::path& p){std::ifstream f(p);Json j;f>>j;return j;}
void Write(const fs::path& p,const Json& j){std::ofstream f(p);f<<j.dump(2);}
wxString W(const fs::path& p){return wxString::FromUTF8(eda::platform::PathToUtf8(p).c_str());}
bool Has(const std::vector<vb::Diagnostic>& ds,const std::string& c){for(const auto& d:ds)if(d.code==c)return true;return false;}
std::pair<Json,Json> Exports(const Json& c){
    Json t={{"type",c.at("shape")=="unknown"?"UNKNOWNDTYPE":"BASICDTYPE"},{"addr","parse-local-only"}};
    if(c.at("shape")=="packed_vector")t["range"]=std::to_string(c.at("left").get<int>())+":"+std::to_string(c.at("right").get<int>());
    Json n={{"type","VAR"},{"name",c.at("name")},{"direction","OUTPUT"},{"isPrimaryIO",true},{"dtypep","parse-local-only"},{"loc","fixture"}};
    Json ast={{"miscsp",Json::array({Json{{"type","TYPETABLE"},{"typesp",Json::array({t})}}})},
              {"modulesp",Json::array({Json{{"name","top"},{"stmtsp",Json::array({n})}}})}};
    Json q={{"direction","output"},{"bits",Json::array()},{"offset",c.at("offset")},{"upto",c.at("upto")}};
    for(int i=0;i<c.at("width");++i)q["bits"].push_back(i+2);
    Json attrs=Json::object();
    if(c.at("shape")=="packed_vector" && c.at("width")==1)attrs["single_bit_vector"]="00000000000000000000000000000001";
    auto name=c.at("name").get<std::string>();
    Json y={{"modules",{{"top",{{"ports",{{name,q}}},{"netnames",{{name,{{"attributes",attrs}}}}}}}}}};
    return {ast,y};
}
eda::cst::ConstraintSheetV2 Sheet(const vb::PortManifest& m){
    eda::cst::ConstraintSheetV2 s;s.manifest=m;s.topModule=m.top_module;s.device="GW1NR-LV9QN88PC6/I5";s.targetProfileId="tang-nano-9k";
    int pins[]={10,11,13,14,15,16};int i=0;
    for(const auto& p:m.ports)for(const auto& r:p.bits){vb::PinBindingV2 b;b.ref=r;b.package_pin=pins[i++%6];b.io_type="LVCMOS18";s.bindings.push_back(b);}
    return s;
}
void RoundTrip(eda::cst::ConstraintSheetV2 s,const fs::path& path){
    auto gui=FromCoreConstraintSheet(s);
    auto rows=MakeGuiBindingRows(s.manifest);Check(rows.size()==s.bindings.size(),"GUI displays every bit");
    for(const auto& row:rows)Check(s.manifest.Contains(row.ref),"GUI row preserves exact slot and HDL index");
    auto back=ToCoreConstraintSheet(gui);
    for(size_t i=0;i<s.bindings.size();++i)Check(back.bindings[i].ref==s.bindings[i].ref,"GUI roundtrip ref");
    wxString err;Check(SaveConstraintSheet(W(path),gui,err),"GUI saves V2: "+err.ToStdString());
    ConstraintSheet loaded;loaded.manifest=s.manifest;Check(LoadConstraintSheet(W(path),loaded,err),"GUI loads V2");
    auto generated=CstGenerator().Generate(loaded);Check(generated.success,"GUI generates CST");
    std::vector<wxString> unmanaged;
    auto imported=CstGenerator().ImportCst(generated.cstContent,s.manifest,unmanaged,err);
    Check(err.empty() && unmanaged.empty() && imported.size()==s.bindings.size(),"CST parses complete roundtrip");
    for(const auto& b:imported){
        auto original=std::find_if(s.bindings.begin(),s.bindings.end(),[&](const auto& a){return a.ref==b.ref;});
        Check(original!=s.bindings.end() && b.packagePin==original->package_pin,"CST ref/pad unchanged");
        Check(generated.cstContent.Contains("\""+b.portName+"\""),"CST explicit shape label");
    }
    Check(eda::cst::ConstraintValidator::Validate(ToCoreConstraintSheet(loaded)).empty(),"V2 structural validation");
    loaded.bindings=imported;
    Check(SaveConstraintSheet(W(path),loaded,err),"imported bindings save through GUI adapter");
    ConstraintSheet reloaded;reloaded.manifest=s.manifest;
    Check(LoadConstraintSheet(W(path),reloaded,err),"imported bindings reload through GUI adapter");
    Check(CstGenerator().Generate(reloaded).success,"import-save-load-generate complete chain");
    // Remove a displayed row: absence must not silently change vector coverage.
    auto missing=ToCoreConstraintSheet(loaded);missing.bindings.erase(missing.bindings.begin());
    Check(Has(eda::cst::ConstraintValidator::Validate(missing),"E_MISSING_BINDING"),"deleted bit is detected");
}
void Ports(const fs::path& root,const fs::path& work){
    auto cases=Read(root/"tests/virtual/k1/fixtures/port-index-cases.json");
    auto electricalFile=root/"registry/electrical/tang-nano-9k.6202.reviewed.v1.json";
    vb::ElectricalProfile electrical;std::vector<vb::Diagnostic> ds;
    Check(vb::LoadElectricalProfile(electricalFile,electrical,ds),"loads real reviewed profile");
    for(const auto& c:cases.at("cases")){
        auto [ast,y]=Exports(c);auto r=vb::PortManifestBuilder::Build(ast,y,"top",std::string(64,'a'));
        if(c.contains("expected_error")){Check(Has(r.diagnostics,c.at("expected_error")),"unknown shape rejected");continue;}
        Check(r.ok(),"builder "+c.at("id").get<std::string>());
        const auto& p=r.manifest.ports[0];
        for(size_t i=0;i<p.bits.size();++i){
            const auto& expected=c.at("expected_indices")[i];
            Check(expected.is_null() ? !p.bits[i].hdl_index : p.bits[i].hdl_index==expected.get<int64_t>(),"true HDL index");
            Check(r.manifest.Label(p.bits[i])==c.at("expected_refs")[i].get<std::string>(),"true HDL label");
            Check(p.bits[i].bit_slot==i,"slot separate from index");
        }
        auto stable=vb::PortManifestBuilder::Build(ast,y,"top",std::string(64,'a'));
        Check(vb::PortManifestBuilder::Serialize(r.manifest)==vb::PortManifestBuilder::Serialize(stable.manifest),"stable manifest");
        Check(vb::PortManifestBuilder::Serialize(r.manifest).dump().find("parse-local-only")==std::string::npos,"no AST pointers persisted");
        auto s=Sheet(r.manifest);Check(eda::cst::ConstraintValidator::Validate(s,&electrical).empty(),"fixture legal LED electrical binding");
        FpgaPinDatabase db;db.device="GW1NR-LV9QN88PC6/I5";db.packageName="QFN88";db.boardRevision="6202";db.electricalProfilePath=W(electricalFile);
        Check(ConstraintValidator(db).Validate(FromCoreConstraintSheet(s),r.manifest).empty(),"actual GUI electrical adapter accepts legal sheet");
        RoundTrip(s,work/(c.at("id").get<std::string>()+".json"));
        auto bad=s;bad.bindings.push_back(bad.bindings[0]);Check(Has(eda::cst::ConstraintValidator::Validate(bad),"E_DUPLICATE_BIT"),"duplicate bit");
        bad=s;bad.bindings[0].ref.hdl_index=999;Check(Has(eda::cst::ConstraintValidator::Validate(bad),"E_PORT_BIT_REF"),"out of range exact lookup");
        bad=s;bad.bindings[0].ref.bit_slot=999;Check(Has(eda::cst::ConstraintValidator::Validate(bad),"E_PORT_BIT_REF"),"forged slot rejected");
        auto label=r.manifest.Label(p.bits[0]);
        auto unknown=eda::cst::CstGenerator().ImportCst("IO_LOC \""+label+"\" 10;\nIO_PORT \""+label+"\" IO_TYPE=LVCMOS18 SLEW_RATE=FAST;",r.manifest);
        Check(Has(unknown.diagnostics,"E_CST_UNMANAGED"),"unknown electrical attribute blocks import");
        auto unmanaged=eda::cst::CstGenerator().ImportCst("PULLUP \""+label+"\";",r.manifest);
        Check(!unmanaged.ok(),"unknown statement fail closed");
        auto wrong=eda::cst::CstGenerator().ImportCst("IO_LOC \"led[999]\" 10;",r.manifest);
        Check(Has(wrong.diagnostics,"E_PORT_BIT_REF"),"import out of range");
        Json legacy={{"schema_version","1.0"},{"top_module","top"},{"bindings",Json::array({Json{{"port","led"},{"bit",0},{"package_pin",10},{"io_type","LVCMOS18"}}})}};
        auto legacyPath=work/"legacy.json";Write(legacyPath,legacy);ds.clear();eda::cst::ConstraintSheetV2 migrated;
        bool canMigrate=p.shape==vb::PortShape::Scalar || (p.width>1 && *p.decl_right==0 && *p.decl_left>0);
        Check(eda::cst::LoadConstraintSheet(legacyPath,r.manifest,migrated,ds)==canMigrate,"legacy ambiguity policy");
        if(!canMigrate)Check(Has(ds,"E_LEGACY_BINDING_AMBIGUOUS"),"explicit migration diagnostic");
        auto conflict=y;conflict["modules"]["top"]["ports"]["led"]["direction"]="input";
        Check(!vb::PortManifestBuilder::Build(ast,conflict,"top",std::string(64,'a')).ok(),"direction conflict");
        conflict=y;conflict["modules"]["top"]["ports"]["led"]["offset"]=123;
        Check(!vb::PortManifestBuilder::Build(ast,conflict,"top",std::string(64,'a')).ok(),"range conflict");
        conflict=y;conflict["modules"]["top"]["netnames"]["led"]["attributes"]["single_bit_vector"]="malformed";
        Check(!vb::PortManifestBuilder::Build(ast,conflict,"top",std::string(64,'a')).ok(),"malformed shape attribute rejected");
        auto version=vb::PortManifestBuilder::Serialize(r.manifest);version["schema"]="v999";
        Check(Has(vb::PortManifestBuilder::Deserialize(version).diagnostics,"E_MANIFEST_VERSION"),"version rejected");
        auto stale=r.manifest;stale.source_sha256=std::string(64,'b');ds.clear();
        Check(!eda::cst::LoadConstraintSheet(work/(c.at("id").get<std::string>()+".json"),stale,migrated,ds),"stale snapshot blocked");
        std::cout<<"PASS "<<c.at("id")<<'\n';
    }
}
void Electrical(const fs::path& root){
    auto fixture=Read(root/"tests/virtual/k1/fixtures/electrical-cases.synthetic.json");
    vb::ElectricalProfile synthetic;synthetic.synthetic=true;synthetic.device={"TEST","TEST","TEST","TEST",{}};
    std::map<std::string,int> ids;int n=0;
    for(auto it=fixture.at("pads").begin();it!=fixture.at("pads").end();++it){ids[it.key()]=++n;synthetic.device.pads[n]={it.value(),true,true,{"LVCMOS18","LVCMOS33"}};}
    for(auto it=fixture.at("banks").begin();it!=fixture.at("banks").end();++it){std::optional<int> v;if(!it.value()["vccio_mv"].is_null())v=it.value()["vccio_mv"].get<int>();synthetic.power.vccio_mv[it.key()]=v;}
    for(auto it=fixture.at("io_rules").begin();it!=fixture.at("io_rules").end();++it){vb::IOStandardProfile rule;rule.directions={vb::Direction::Input,vb::Direction::Output};rule.vccio_allowed_mv=it.value()["vccio_allowed_mv"].get<std::set<int>>();rule.pull_modes={"none"};synthetic.standards[it.key()]=rule;}
    auto [ast,y]=Exports({{"shape","scalar"},{"width",1},{"offset",0},{"upto",0},{"name","led"}});
    auto m=vb::PortManifestBuilder::Build(ast,y,"top",std::string(64,'a')).manifest;
    for(const auto& c:fixture.at("cases")){
        auto current=m;current.ports[0].direction=c.at("direction")=="inout"?vb::Direction::InOut:vb::Direction::Output;
        vb::PinBindingV2 b;b.ref=current.ports[0].bits[0];b.package_pin=ids[c.at("pad")];if(!c.at("io_type").is_null())b.io_type=c.at("io_type");
        auto ds=vb::ValidateElectrical(synthetic,{"TEST","TEST","TEST"},current,{b});
        Check(c.at("expected")=="OK"?ds.empty():Has(ds,c.at("expected")),"synthetic "+c.at("id").get<std::string>());
        std::cout<<"PASS synthetic "<<c.at("id")<<'\n';
    }
    vb::ElectricalProfile p;std::vector<vb::Diagnostic> ds;auto file=root/"registry/electrical/tang-nano-9k.6202.reviewed.v1.json";
    Check(vb::LoadElectricalProfile(file,p,ds),"production profile loads");
    Check(!p.synthetic,"production data never synthetic");
    auto raw=Read(file);
    for(const auto& source:raw.at("provenance"))Check(eda::platform::Sha256FileHex(root/eda::platform::PathFromUtf8(source.at("archived_file")))==source.at("sha256").get<std::string>(),"official archived PDF SHA256");
    int pins[]={10,11,13,14,15,16,3,4,52,17,18};
    for(int pad:pins){
        auto current=m;current.ports[0].direction=(pad==3||pad==4||pad==52||pad==18)?vb::Direction::Input:vb::Direction::Output;
        vb::PinBindingV2 b;b.ref=current.ports[0].bits[0];b.package_pin=pad;b.io_type=(pad==52||pad==17||pad==18)?"LVCMOS33":"LVCMOS18";
        auto valid=vb::ValidateElectrical(p,{p.device.device,p.device.package,p.device.board_revision},current,{b});
        Check(valid.empty(),"production pin "+std::to_string(pad));
        if(pad==10){
            auto wrong=b;wrong.io_type="LVCMOS33";
            auto altered=p;
            Check(Has(vb::ValidateElectrical(p,{p.device.device,p.device.package,p.device.board_revision},current,{wrong}),"E_IO_VCCIO_MISMATCH"),"LED incorrect VCCIO");
            // Check original profile too: IO_TYPE known but incompatible with the supply.
            Check(!vb::ValidateElectrical(p,{p.device.device,p.device.package,p.device.board_revision},current,{wrong}).empty(),"LED 3.3V rejected");
            altered=p;altered.power.vccio_mv["3"]=std::nullopt;
            Check(Has(vb::ValidateElectrical(altered,{p.device.device,p.device.package,p.device.board_revision},current,{b}),"E_VCCIO_UNKNOWN"),"missing supply");
            altered=p;altered.device.pads[pad].vendor_bank_id.clear();
            Check(Has(vb::ValidateElectrical(altered,{p.device.device,p.device.package,p.device.board_revision},current,{b}),"E_BANK_UNKNOWN"),"missing bank");
            auto b2=wrong;b2.package_pin=11;b2.ref=b.ref;
            Check(Has(vb::ValidateElectrical(p,{p.device.device,p.device.package,p.device.board_revision},current,{b,b2}),"E_BANK_STANDARD_CONFLICT"),"same Bank conflicting standards");
            wrong=b;wrong.pull_up=true;
            Check(Has(vb::ValidateElectrical(p,{p.device.device,p.device.package,p.device.board_revision},current,{wrong}),"E_PULL_UNSUPPORTED"),"unreviewed internal pull blocked");
            wrong=b;wrong.drive="24";
            Check(Has(vb::ValidateElectrical(p,{p.device.device,p.device.package,p.device.board_revision},current,{wrong}),"E_DRIVE_UNSUPPORTED"),"drive rule blocked");
        }
    }
    vb::PinBindingV2 unknown;unknown.ref=m.ports[0].bits[0];unknown.package_pin=19;unknown.io_type="LVCMOS33";
    Check(Has(vb::ValidateElectrical(p,{p.device.device,p.device.package,p.device.board_revision},m,{unknown}),"E_BANK_UNKNOWN"),"unreviewed pad closed");
    Check(Has(vb::ValidateElectrical(p,{"wrong",p.device.package,p.device.board_revision},m,{unknown}),"E_DEVICE_IDENTITY"),"identity first");
}
void Real(const fs::path& root,const fs::path& work){
    FpgaPinDatabase db;ConstraintValidator guiValidator(db);wxString error;
    auto path=work/"actual/ports.json";
    auto m=guiValidator.ExtractPortsFromYosysJson(W(path),"k1_ports",error);
    Check(!m.empty(),"GUI reads actual tool cross-checked sidecar: "+error.ToStdString());
    Check(m.ports.size()==6,"all actual tool ports");
    auto s=Sheet(m);int pin=1;for(auto& b:s.bindings)b.package_pin=pin++;
    // This 17-bit indexing fixture exceeds the 11-pad reviewed M0 board subset.
    // Structural/CST coverage is tested here; real electrical coverage is above.
    RoundTrip(s,work/"actual-bindings.json");
    auto side=Read(fs::path(path.string()+".ports.v2.json"));auto ast=Read(work/"actual/ports-declarations/Vk1_ports.tree.json");auto y=Read(path);
    for(const auto& p:m.ports)if(p.rtl_name=="one_bit_vector")Check(p.shape==vb::PortShape::PackedVector && p.bits[0].hdl_index==0,"actual [0:0] AST shape");
    auto bad=y;bad["modules"]["k1_ports"]["ports"]["up"]["upto"]=0;
    Check(!vb::PortManifestBuilder::Build(ast,bad,"k1_ports",m.source_sha256).ok(),"actual ascending conflict rejected");
    (void)root;
}
}
int main(int argc,char** argv){
    try{if(argc!=4)throw std::runtime_error("mode root work required");
        auto root=eda::platform::PathFromUtf8(argv[2]),work=eda::platform::PathFromUtf8(argv[3]);fs::create_directories(work);
        std::string mode=argv[1];if(mode=="ports")Ports(root,work);else if(mode=="electrical")Electrical(root);else if(mode=="real")Real(root,work);else throw std::runtime_error("mode");
        std::cout<<"PASS "<<checks<<" checks\n";return 0;
    }catch(const std::exception& e){std::cerr<<"FAIL after "<<checks<<" checks: "<<e.what()<<'\n';return 1;}
}
