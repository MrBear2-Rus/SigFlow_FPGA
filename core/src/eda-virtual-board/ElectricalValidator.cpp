#include "eda-platform/Platform.h"
#include "ElectricalValidator.h"
#include <eda/api/Types.h>
#include <fstream>

namespace eda::virtual_board {
bool LoadElectricalProfile(const std::filesystem::path& path, ElectricalProfile& result,
                           std::vector<Diagnostic>& errors, bool allow_synthetic) {
    try {
        std::ifstream f(path); Json j; f >> j;
        ElectricalProfile p; p.schema = j.at("schema"); p.synthetic = j.at("synthetic");
        if (p.schema != "sigflow.electrical.v1" || (p.synthetic && !allow_synthetic))
            throw std::runtime_error("E_ELECTRICAL_PROFILE_VERSION_OR_SYNTHETIC");
        p.device.device = j.at("device"); p.device.package = j.at("package");
        p.device.vendor_package = j.at("vendor_package"); p.device.board_revision = j.at("board_revision");
        for (const auto& s : j.at("provenance")) {
            Provenance v; v.id = s.at("id"); v.document_version = s.at("document_version");
            v.url = s.at("url"); v.sha256 = s.at("sha256"); v.physical_pages = s.at("physical_pages").get<std::vector<int>>();
            if (v.id.empty() || v.sha256.size() != 64 || v.physical_pages.empty())
                throw std::runtime_error("E_PROVENANCE_MISSING");
            p.provenance.push_back(v);
        }
        if (!p.synthetic && p.provenance.empty()) throw std::runtime_error("E_PROVENANCE_MISSING");
        for (const auto& s : j.at("pads")) {
            DevicePad d; d.reviewed = s.at("reviewed"); d.available = s.at("available");
            if (!s.at("vendor_bank_id").is_null()) d.vendor_bank_id = s.at("vendor_bank_id");
            d.allowed_io_types = s.at("allowed_io_types").get<std::set<std::string>>();
            if (!p.device.pads.emplace(s.at("pin").get<int>(), d).second) throw std::runtime_error("E_PAD_DUPLICATE");
        }
        for (auto it = j.at("banks").begin(); it != j.at("banks").end(); ++it) {
            std::optional<int> v;
            if (!it.value().at("vccio_mv").is_null()) v = it.value().at("vccio_mv").get<int>();
            p.power.vccio_mv[it.key()] = v;
        }
        for (auto it = j.at("io_standards").begin(); it != j.at("io_standards").end(); ++it) {
            IOStandardProfile s;
            for (const std::string d : it.value().at("directions")) {
                if (d != "input" && d != "output" && d != "inout") throw std::runtime_error("E_DIRECTION");
                s.directions.insert(d == "input" ? Direction::Input : d == "output" ? Direction::Output : Direction::InOut);
            }
            s.vccio_allowed_mv = it.value().at("vccio_allowed_mv").get<std::set<int>>();
            s.drive_allowed_ma = it.value().at("drive_allowed_ma").get<std::set<int>>();
            s.pull_modes = it.value().at("pull_modes").get<std::set<std::string>>();
            p.standards[it.key()] = s;
        }
        for (const auto& e : j.at("endpoints")) {
            ExternalEndpointProfile v; v.role = e.at("role"); v.voltage_mv = e.at("voltage_mv");
            v.pull_modes = e.at("pull_modes").get<std::set<std::string>>();
            p.endpoints.emplace(e.at("pin").get<int>(), v);
        }
        result = std::move(p); return true;
    } catch(const std::exception& e) {
        errors.push_back({"E_ELECTRICAL_PROFILE", e.what(), eda::platform::PathToUtf8(path)}); return false;
    }
}
std::vector<Diagnostic> ValidateElectrical(const ElectricalProfile& p, const ElectricalIdentity& id,
                                          const PortManifest& m, const std::vector<PinBindingV2>& bindings) {
    std::vector<Diagnostic> out;
    if (id.device != p.device.device || id.package != p.device.package || id.board_revision != p.device.board_revision) {
        out.push_back({"E_DEVICE_IDENTITY", "device/package/board_revision mismatch", "identity"}); return out;
    }
    std::map<std::string, std::set<int>> bank_required;
    for (size_t i=0; i<bindings.size(); ++i) {
        const auto& b = bindings[i];
        auto error = [&](const std::string& code, const std::string& msg) {
            Diagnostic d{code,msg,"bindings",int(i)};
            d.port_ref=m.Label(b.ref); d.package_pin=b.package_pin; d.requested_io_type=b.io_type;
            auto padInfo=p.device.pads.find(b.package_pin);
            if(padInfo!=p.device.pads.end())d.vendor_bank_id=padInfo->second.vendor_bank_id;
            auto supply=p.power.vccio_mv.find(d.vendor_bank_id);
            if(supply!=p.power.vccio_mv.end())d.vccio_mv=supply->second;
            d.remediation="Choose a reviewed pad, its matching IO_TYPE and endpoint direction; remove unsupported pull/drive settings.";
            for(const auto& source:p.provenance){
                std::string ref=source.id+" ("+source.document_version+", PDF pages";
                for(auto page:source.physical_pages)ref+=" "+std::to_string(page);
                ref+=")";d.provenance_refs.push_back(ref);
            }
            d.message+="; port="+d.port_ref+", pad="+std::to_string(b.package_pin)+", Bank="+
                (d.vendor_bank_id.empty()?"unknown":d.vendor_bank_id)+", VCCIO="+
                (d.vccio_mv?std::to_string(*d.vccio_mv):"unknown")+"mV, IO_TYPE="+b.io_type+". "+d.remediation;
            for(const auto& ref:d.provenance_refs)d.message+=" ["+ref+"]";
            out.push_back(std::move(d));
        };
        auto pad = p.device.pads.find(b.package_pin);
        if (pad == p.device.pads.end() || !pad->second.reviewed || pad->second.vendor_bank_id.empty()) {
            error("E_BANK_UNKNOWN", "Pad/Bank has not been reviewed"); continue;
        }
        const auto& d = pad->second;
        auto v = p.power.vccio_mv.find(d.vendor_bank_id);
        if (v == p.power.vccio_mv.end() || !v->second) { error("E_VCCIO_UNKNOWN","Bank supply unknown"); continue; }
        auto standard = p.standards.find(b.io_type);
        if (standard == p.standards.end()) { error("E_IO_STANDARD_UNKNOWN","Explicit reviewed IO_TYPE required"); continue; }
        const auto& rule = standard->second;
        if (rule.vccio_allowed_mv.size() == 1) bank_required[d.vendor_bank_id].insert(*rule.vccio_allowed_mv.begin());
        auto port = m.Find(b.ref.port_id);
        if (!port || !m.Contains(b.ref)) { error("E_PORT_BIT_REF","Invalid port reference"); continue; }
        if (!d.available || !rule.directions.count(port->direction)) {
            error("E_ELECTRICAL_UNSUPPORTED","Pad direction or standard capability unsupported"); continue;
        }
        if (!rule.vccio_allowed_mv.count(*v->second)) { error("E_IO_VCCIO_MISMATCH","IO_TYPE incompatible with board VCCIO"); continue; }
        if (!d.allowed_io_types.count(b.io_type)) { error("E_ELECTRICAL_UNSUPPORTED","Pad standard capability unreviewed"); continue; }
        if (b.pull_up && b.pull_down) { error("E_PULL_CONFLICT","Pull-up and pull-down are mutually exclusive"); continue; }
        std::string pull = b.pull_up ? "up" : b.pull_down ? "down" : "none";
        if (!rule.pull_modes.count(pull)) { error("E_PULL_UNSUPPORTED","Pull mode not reviewed"); continue; }
        if (!b.drive.empty()) {
            try {
                size_t used; int drive = std::stoi(b.drive,&used);
                if (used != b.drive.size() || port->direction == Direction::Input || !rule.drive_allowed_ma.count(drive))
                    throw std::runtime_error("drive");
            } catch (...) { error("E_DRIVE_UNSUPPORTED","Drive strength not reviewed for this direction"); continue; }
        }
        auto ep = p.endpoints.find(b.package_pin);
        if (ep != p.endpoints.end()) {
            const auto& e = ep->second;
            if (e.voltage_mv != *v->second) { error("E_ENDPOINT_VOLTAGE","Endpoint voltage incompatible"); continue; }
            if ((e.role == "source" && port->direction != Direction::Input) ||
                (e.role == "sink" && port->direction != Direction::Output)) {
                error("E_ENDPOINT_DIRECTION","Endpoint and FPGA drive directions conflict"); continue;
            }
            if (!e.pull_modes.count(pull)) error("E_ENDPOINT_PULL","Pull conflicts with board wiring");
        }
    }
    for (const auto& [bank, voltages] : bank_required) if (voltages.size() > 1)
        out.push_back({"E_BANK_STANDARD_CONFLICT","Bound standards require different supplies in Bank " + bank,"banks"});
    return out;
}
}
