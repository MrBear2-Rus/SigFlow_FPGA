#include "eda-platform/Platform.h"
#include "FpgaPinData.h"

#include "PinDatabaseStore.h"

#include "platform/PlatformPaths.h"

#include <wx/filefn.h>
#include <wx/filename.h>
#include <wx/stdpaths.h>

#include <filesystem>
#include <vector>

namespace {

std::vector<wxString> g_knownIOTypes;

std::filesystem::path FindPinDatabaseFile()
{
    const wxString executableDirectory =
        wxFileName(wxStandardPaths::Get().GetExecutablePath()).GetPath();

    // 1) 随可执行文件分发：<exeDir>/target-profiles/tang-nano-9k.pins.json
    const wxString beside =
        executableDirectory + wxFileName::GetPathSeparator() + "target-profiles" +
        wxFileName::GetPathSeparator() + "tang-nano-9k.pins.json";
    if (wxFileExists(beside)) {
        return sigflow::platform::Utf8Path(beside);
    }

    // 2) 仓库内：向上查找 main/fpga/target-profiles
    const auto probe = [](const wxString& directory) -> wxString {
        const wxString candidate =
            directory + wxFileName::GetPathSeparator() + "main" + wxFileName::GetPathSeparator() +
            "fpga" + wxFileName::GetPathSeparator() + "target-profiles" +
            wxFileName::GetPathSeparator() + "tang-nano-9k.pins.json";
        return wxFileExists(candidate) ? candidate : wxString();
    };
    wxString found = sigflow::platform::WalkUpDirectories(executableDirectory, 8, probe);
    if (found.IsEmpty()) {
        found = sigflow::platform::WalkUpDirectories(wxGetCwd(), 8, probe);
    }
    if (found.IsEmpty()) return {};
    return sigflow::platform::Utf8Path(found);
}

} // namespace

void ApplyReservedPins(FpgaPinDatabase& db)
{
    const int reservedPins[] = { 5, 6, 7, 8 };
    const char* jtagReasons[] = { "JTAG TMS", "JTAG TCK", "JTAG TDI", "JTAG TDO" };
    for (int i = 0; i < 4; ++i) {
        auto* pin = const_cast<PackagePin*>(db.FindPin(reservedPins[i]));
        if (pin) {
            pin->available = false;
            pin->reservedReason = jtagReasons[i];
        }
    }
}

// P1-5：引脚库改为从 target-profiles/tang-nano-9k.pins.json 加载（取代硬编码表）。
const FpgaPinDatabase& GetQFN88PinDatabase()
{
    static FpgaPinDatabase db;
    static bool initialized = false;
    if (!initialized) {
        eda::target::PinDatabase loaded;
        std::string error;
        const std::filesystem::path file = FindPinDatabaseFile();
        if (!file.empty() &&
            eda::target::PinDatabaseStore::LoadFile(file, loaded, error)) {
            db.device = wxString::FromUTF8(loaded.device.c_str());
            db.packageName = wxString::FromUTF8(loaded.packageName.c_str());
            db.boardRevision = wxString::FromUTF8(loaded.boardRevision.c_str());
            // Resolve the declared registry reference beside a distribution or
            // repository root, without substituting a different board profile.
            auto parent = file.parent_path();
            for (int depth=0; depth<8 && !parent.empty(); ++depth) {
                auto candidate = parent / eda::platform::PathFromUtf8(loaded.electricalProfileRef);
                if (!loaded.electricalProfileRef.empty() && std::filesystem::is_regular_file(candidate)) {
                    db.electricalProfilePath = wxString::FromUTF8(sigflow::platform::PathToUtf8(candidate).c_str()); break;
                }
                auto next = parent.parent_path(); if (next == parent) break; parent=next;
            }
            db.pins.reserve(loaded.pins.size());
            for (const auto& pin : loaded.pins) {
                PackagePin entry;
                entry.pinNumber = pin.pin;
                entry.bank = pin.bank;
                entry.ioLocation = wxString::FromUTF8(pin.ioLocation.c_str());
                entry.available = pin.available;
                entry.reservedReason = wxString::FromUTF8(pin.reservedReason.c_str());
                entry.defaultIOType = wxString::FromUTF8(pin.defaultIOType.c_str());
                db.pins.push_back(std::move(entry));
            }
            db.boardResources.reserve(loaded.boardResources.size());
            for (const auto& resource : loaded.boardResources) {
                BoardResource entry;
                entry.alias = wxString::FromUTF8(resource.alias.c_str());
                entry.packagePin = resource.pin;
                entry.description = wxString::FromUTF8(resource.description.c_str());
                entry.resourceType = wxString::FromUTF8(resource.type.c_str());
                db.boardResources.push_back(std::move(entry));
            }
            g_knownIOTypes.clear();
            for (const auto& ioType : loaded.ioTypes) {
                g_knownIOTypes.push_back(wxString::FromUTF8(ioType.c_str()));
            }
        }
        initialized = true;
    }
    return db;
}

const PackagePin* FpgaPinDatabase::FindPin(int pinNumber) const {
    for (const auto& p : pins) {
        if (p.pinNumber == pinNumber) return &p;
    }
    return nullptr;
}

const BoardResource* FpgaPinDatabase::FindResourceByPin(int pinNumber) const {
    for (const auto& r : boardResources) {
        if (r.packagePin == pinNumber) return &r;
    }
    return nullptr;
}

std::vector<const BoardResource*> FpgaPinDatabase::FindResourcesByType(const wxString& type) const {
    std::vector<const BoardResource*> result;
    for (const auto& r : boardResources) {
        if (r.resourceType == type) {
            result.push_back(&r);
        }
    }
    return result;
}

std::vector<int> FpgaPinDatabase::GetAvailablePins() const {
    std::vector<int> result;
    for (const auto& p : pins) {
        if (p.available) result.push_back(p.pinNumber);
    }
    return result;
}

bool FpgaPinDatabase::IsPinAvailable(int pinNumber) const {
    const auto* p = FindPin(pinNumber);
    return p && p->available;
}

const std::vector<wxString>& GetKnownIOTypes() {
    GetQFN88PinDatabase();  // 确保已加载
    return g_knownIOTypes;
}
