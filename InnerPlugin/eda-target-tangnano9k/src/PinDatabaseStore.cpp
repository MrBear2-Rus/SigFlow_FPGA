#include "PinDatabaseStore.h"

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

} // namespace

bool PinDatabaseStore::Parse(const std::string& json, PinDatabase& database, std::string& error) {
    Json root;
    try {
        root = Json::parse(json);
    } catch (const std::exception& parseError) {
        error = std::string("invalid pin database JSON: ") + parseError.what();
        return false;
    }
    if (!root.is_object()) {
        error = "pin database must be a JSON object";
        return false;
    }

    PinDatabase parsed;
    parsed.device = OptionalString(root, "device");
    parsed.packageName = OptionalString(root, "package");
    const std::string defaultIoType = OptionalString(root, "default_io_type", "LVCMOS33");

    if (root.contains("pins") && root["pins"].is_array()) {
        for (const auto& entry : root["pins"]) {
            if (!entry.is_object() || !entry.contains("pin")) continue;
            PinEntry pin;
            pin.pin = entry.value("pin", 0);
            pin.bank = entry.value("bank", 0);
            pin.ioLocation = OptionalString(entry, "io");
            pin.available = entry.value("available", true);
            pin.reservedReason = OptionalString(entry, "reserved");
            pin.defaultIOType = defaultIoType;
            parsed.pins.push_back(std::move(pin));
        }
    }
    if (root.contains("board_resources") && root["board_resources"].is_array()) {
        for (const auto& entry : root["board_resources"]) {
            if (!entry.is_object()) continue;
            BoardResourceEntry resource;
            resource.alias = OptionalString(entry, "alias");
            resource.pin = entry.value("pin", 0);
            resource.description = OptionalString(entry, "description");
            resource.type = OptionalString(entry, "type");
            if (!resource.alias.empty()) parsed.boardResources.push_back(std::move(resource));
        }
    }
    if (root.contains("io_types") && root["io_types"].is_array()) {
        for (const auto& entry : root["io_types"]) {
            if (entry.is_string()) parsed.ioTypes.push_back(entry.get<std::string>());
        }
    }

    if (parsed.pins.empty()) {
        error = "pin database has no pins";
        return false;
    }
    database = std::move(parsed);
    return true;
}

bool PinDatabaseStore::LoadFile(const std::filesystem::path& file, PinDatabase& database,
                                std::string& error) {
    std::ifstream input(file, std::ios::binary);
    if (!input) {
        error = "pin database not found: " + file.string();
        return false;
    }
    const std::string content((std::istreambuf_iterator<char>(input)),
                              std::istreambuf_iterator<char>());
    return Parse(content, database, error);
}

} // namespace target
} // namespace eda
