#include "TargetProfileStore.h"

#include <eda/api/Types.h>

#include <fstream>
#include <iterator>

namespace eda {
namespace target {
namespace {

std::string OptionalString(const Json& object, const char* key, const std::string& fallback = {}) {
    if (object.is_object() && object.contains(key) && object[key].is_string()) {
        return object[key].get<std::string>();
    }
    return fallback;
}

int OptionalInt(const Json& object, const char* key, int fallback) {
    if (object.is_object() && object.contains(key) && object[key].is_number_integer()) {
        return object[key].get<int>();
    }
    return fallback;
}

} // namespace

bool TargetProfileStore::Parse(const std::string& json, TargetProfile& profile,
                               std::string& error) {
    Json root;
    try {
        root = Json::parse(json);
    } catch (const std::exception& parseError) {
        error = std::string("invalid target profile JSON: ") + parseError.what();
        return false;
    }
    if (!root.is_object()) {
        error = "target profile must be a JSON object";
        return false;
    }

    TargetProfile parsed;
    parsed.id = OptionalString(root, "id");
    if (parsed.id.empty()) {
        error = "target profile is missing 'id'";
        return false;
    }
    parsed.version = OptionalString(root, "version");
    parsed.displayName = OptionalString(root, "display_name");

    const Json yosys = root.value("yosys", Json::object());
    parsed.yosysFamily = OptionalString(yosys, "family");

    const Json nextpnr = root.value("nextpnr", Json::object());
    parsed.device = OptionalString(nextpnr, "device");
    parsed.family = OptionalString(nextpnr, "family");

    const Json loader = root.value("openfpgaloader", Json::object());
    parsed.programmerBoard = OptionalString(loader, "board");

    const Json tracebridge = root.value("tracebridge", Json::object());
    parsed.uartTxPin = OptionalInt(tracebridge, "uart_tx_pin", -1);
    parsed.uartRxPin = OptionalInt(tracebridge, "uart_rx_pin", -1);
    parsed.debugReset = OptionalString(tracebridge, "debug_reset");
    parsed.defaultBaud = OptionalInt(tracebridge, "default_baud", 0);

    profile = std::move(parsed);
    return true;
}

bool TargetProfileStore::LoadFile(const std::filesystem::path& file, TargetProfile& profile,
                                  std::string& error) {
    std::ifstream input(file, std::ios::binary);
    if (!input) {
        error = "target profile not found: " + file.string();
        return false;
    }
    const std::string content((std::istreambuf_iterator<char>(input)),
                              std::istreambuf_iterator<char>());
    if (!Parse(content, profile, error)) return false;
    profile.sourcePath = file.generic_string();
    return true;
}

bool TargetProfileStore::LoadById(const std::filesystem::path& directory, const std::string& id,
                                  TargetProfile& profile, std::string& error) {
    if (id.empty()) {
        error = "target profile id is empty";
        return false;
    }
    const std::filesystem::path file = directory / (id + ".json");
    return LoadFile(file, profile, error);
}

} // namespace target
} // namespace eda
