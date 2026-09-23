#include <eda/api/target_profile.hpp>

#include "PinDatabaseStore.h"
#include "TargetProfileStore.h"

#include <filesystem>
#include <iostream>
#include <string>

namespace fs = std::filesystem;

namespace {

int g_failures = 0;

void Check(bool ok, const char* msg) {
    if (ok) {
        std::cout << "  ok: " << msg << "\n";
    } else {
        ++g_failures;
        std::cout << "  FAIL: " << msg << "\n";
    }
}

} // namespace

int main(int argc, char** argv) {
    const fs::path profileFile = argc > 1 ? argv[1] : "";

    // 解析仓库内置的 eda.target-profile.v1（取代硬编码 profile）。
    {
        eda::TargetProfile profile;
        std::string error;
        Check(eda::target::TargetProfileStore::LoadFile(profileFile, profile, error),
              "loads tang-nano-9k target profile file");
        Check(profile.id == "tang-nano-9k", "profile id");
        Check(profile.device == "GW1NR-LV9QN88PC6/I5", "profile device");
        Check(profile.family == "GW1N-9C", "profile family");
        Check(profile.yosysFamily == "gw1n", "profile yosys family");
        Check(profile.programmerBoard == "tangnano9k", "profile programmer board");
        Check(profile.uartTxPin == 17 && profile.uartRxPin == 18, "profile debug uart pins");
        Check(profile.defaultBaud == 921600, "profile default baud");
    }

    // LoadById 在目录下查找 <id>.json。
    {
        eda::TargetProfile profile;
        std::string error;
        Check(eda::target::TargetProfileStore::LoadById(profileFile.parent_path(),
                                                        "tang-nano-9k", profile, error),
              "loads profile by id from directory");
        Check(profile.id == "tang-nano-9k", "profile-by-id id");
    }

    // 内联最小 JSON。
    {
        eda::TargetProfile profile;
        std::string error;
        const std::string json =
            R"({"id":"mini","version":"0.1","nextpnr":{"device":"DEV","family":"FAM"}})";
        Check(eda::target::TargetProfileStore::Parse(json, profile, error),
              "parses minimal inline profile");
        Check(profile.device == "DEV" && profile.family == "FAM", "inline profile fields");
        Check(profile.uartTxPin == -1 && profile.defaultBaud == 0, "missing optional fields");
    }

    // 错误路径。
    {
        eda::TargetProfile profile;
        std::string error;
        Check(!eda::target::TargetProfileStore::Parse("not json", profile, error),
              "invalid json rejected");
        Check(!eda::target::TargetProfileStore::Parse("{}", profile, error),
              "profile without id rejected");
        Check(!eda::target::TargetProfileStore::LoadFile("definitely-missing.json", profile, error),
              "missing profile file rejected");
    }

    // P1-5 收尾：引脚库数据外提。
    {
        const fs::path pinsFile = argc > 2 ? argv[2] : "";
        eda::target::PinDatabase database;
        std::string error;
        Check(eda::target::PinDatabaseStore::LoadFile(pinsFile, database, error),
              "loads pin database JSON");
        Check(database.pins.size() == 88, "pin database has 88 pins");
        Check(database.boardResources.size() == 18, "pin database has 18 board resources");
        Check(database.ioTypes.size() == 21, "pin database has 21 IO types");
        const eda::target::PinEntry* pin10 = nullptr;
        const eda::target::PinEntry* pin56 = nullptr;
        for (const auto& pin : database.pins) {
            if (pin.pin == 10) pin10 = &pin;
            if (pin.pin == 56) pin56 = &pin;
        }
        Check(pin10 != nullptr && pin10->available, "pin 10 available");
        Check(pin56 != nullptr && !pin56->available && pin56->reservedReason == "JTAG TCK",
              "pin 56 reserved as JTAG TCK");
    }

    std::cout << (g_failures == 0 ? "ALL PASS" : "FAILURES") << "\n";
    return g_failures == 0 ? 0 : 1;
}
