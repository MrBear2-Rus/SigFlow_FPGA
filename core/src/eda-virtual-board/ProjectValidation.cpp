#include "ProjectValidation.h"
#include "PortManifestBuilder.h"
#include "ElectricalValidator.h"
#include "BoardTopology.h"
#include "ConstraintSheet.h"
#include "eda-platform/Platform.h"
#include <fstream>
#include <regex>
#include <sstream>
namespace eda::virtual_board {
namespace {
Json Ref(const PortBitRef& r){return {{"port_id",r.port_id},{"hdl_index",r.hdl_index?Json(*r.hdl_index):Json(nullptr)},{"bit_slot",r.bit_slot}};}
std::string Suggestion(const std::string& code){
    if(code=="E_IO_VCCIO_MISMATCH")return "按此 Bank 的实际 VCCIO 选择兼容的 IO_TYPE；不要回退为全局 LVCMOS33。";
    if(code=="E_BANK_UNKNOWN" || code=="E_VCCIO_UNKNOWN")return "核对所选板修订的核准 Pad/Bank/供电资料，补齐数据后重建。";
    if(code.find("CLOCK")!=std::string::npos)return "将显式 clock.port_ref 的端口位通过 CST 接到板卡晶振 Pad。";
    if(code=="E_MISSING_BINDING")return "在 Pin Binding 中绑定该真实 HDL 位；教学漏绑输出需显式开启 allow_unbound_outputs。";
    if(code=="E_CST_UNMANAGED")return "删除或改写不受支持的 CST 语句；不得忽略会影响连线或电气的命令。";
    return "在 Pin Binding 中核对端口真实下标、物理脚、方向与 I/O 属性，并重新构建。";
}
}
Json ValidateVirtualProject(const Json& config,const PortManifest& manifest,bool synthetic){
    Json out={{"schema","sigflow.virtual-project-validation.v1"},{"ok",false},{"diagnostics",Json::array()},{"manifest",PortManifestBuilder::Serialize(manifest)},{"binding_rows",Json::array()}};
    ElectricalProfile profile;std::vector<PinBindingV2> bindings;std::vector<std::string> lines;
    auto add=[&](const Diagnostic& d){
        const PinBindingV2* binding=nullptr;const PortBitRef* ref=nullptr;std::string label;
        if(d.field=="bindings" && d.row>=0 && size_t(d.row)<bindings.size())binding=&bindings.at(d.row);
        if(binding){ref=&binding->ref;label=manifest.Label(*ref);}
        if(d.field=="line" && d.row>0 && size_t(d.row)<=lines.size()){
            std::smatch match;if(std::regex_search(lines[d.row-1],match,std::regex("\"([^\"]+)\"")))label=match[1];
        }
        if(label.empty() && d.message.starts_with("Missing "))label=d.message.substr(8);
        for(const auto& p:manifest.ports)for(const auto& bit:p.bits)if(manifest.Label(bit)==label)ref=&bit;
        if(!binding && ref)for(const auto& b:bindings)if(b.ref==*ref)binding=&b;
        int pad=binding?binding->package_pin:-1;std::string bank;Json vccio=nullptr;
        if(auto p=profile.device.pads.find(pad);p!=profile.device.pads.end()){
            bank=p->second.vendor_bank_id;auto supply=profile.power.vccio_mv.find(bank);if(supply!=profile.power.vccio_mv.end() && supply->second)vccio=*supply->second;
        }
        out["diagnostics"].push_back({{"code",d.code},{"message",d.message},{"port_ref",label},{"port_bit_ref",ref?Ref(*ref):Json(nullptr)},
            {"package_pin",pad>0?Json(pad):Json(nullptr)},{"bank_id",bank},{"vccio_mv",vccio},{"row",d.row},{"field",d.field},{"suggestion",Suggestion(d.code)}});
    };
    try{
        if(config.at("schema")!="sigflow.virtual-session.v1")throw std::runtime_error("E_RUNNER_SCHEMA");
        std::vector<Diagnostic> errors;
        if(!LoadElectricalProfile(platform::PathFromUtf8(config.at("electrical_profile_path")),profile,errors,synthetic)){
            for(const auto& d:errors)add(d);return out;
        }
        std::ifstream input(platform::PathFromUtf8(config.at("cst_path")));if(!input)throw std::runtime_error("E_CST_FILE");
        std::string text((std::istreambuf_iterator<char>(input)),{});std::istringstream split(text);for(std::string line;std::getline(split,line);)lines.push_back(line);
        auto imported=eda::cst::CstGenerator().ImportCst(text,manifest);bindings=imported.bindings;for(const auto& d:imported.diagnostics)add(d);
        eda::cst::ConstraintSheetV2 sheet;sheet.manifest=manifest;sheet.bindings=bindings;sheet.topModule=manifest.top_module;
        const auto& identity=config.at("identity");sheet.device=identity.at("device");sheet.package=identity.at("package");sheet.boardRevision=identity.at("board_revision");
        bool allow=config.value("allow_unbound_outputs",false);
        for(const auto& d:eda::cst::ConstraintValidator::Validate(sheet,&profile)){
            bool permitted=false;if(allow && d.code=="E_MISSING_BINDING" && d.row<0)for(const auto& p:manifest.ports)if(p.direction==Direction::Output)
                for(const auto& bit:p.bits)if(d.message=="Missing "+manifest.Label(bit))permitted=true;
            if(!permitted)add(d);
        }
        if(config.contains("board_path")){
            auto board=BoardTopology::Load(platform::PathFromUtf8(config.at("board_path")),profile);
            const auto& clock=config.at("clock");std::string clockLabel=clock.value("port_ref",std::string());bool clockBound=false;
            for(const auto& b:bindings)if(manifest.Label(b.ref)==clockLabel && b.package_pin==board->ClockPad() && manifest.Find(b.ref.port_id)->direction==Direction::Input)clockBound=true;
            if(!clockBound){Diagnostic d{"E_CLOCK_BINDING","Required clock is not bound to oscillator Pad","bindings"};
                for(size_t i=0;i<bindings.size();++i)if(manifest.Label(bindings[i].ref)==clockLabel)d.row=int(i);add(d);}
            if(clock.at("package_pin")!=board->ClockPad() || std::stoull(clock.at("frequency_hz").get<std::string>())!=board->Frequency())add({"E_BOARD_CLOCK_BINDING","Clock parameters differ from board","clock"});
        }
        for(const auto& p:manifest.ports)for(const auto& bit:p.bits){Json row={{"label",manifest.Label(bit)},{"ref",Ref(bit)},{"direction",p.direction==Direction::Input?"input":"output"},{"package_pin",nullptr},{"io_type",""}};
            for(const auto& b:bindings)if(b.ref==bit){row["package_pin"]=b.package_pin;row["io_type"]=b.io_type;}out["binding_rows"].push_back(std::move(row));}
        out["ok"]=out.at("diagnostics").empty();return out;
    }catch(const std::exception& e){std::string message=e.what();auto code=message.starts_with("E_")?message.substr(0,message.find_first_of(": ")):"E_PROJECT_CONFIGURATION";add({code,message,"project"});return out;}
}
}
