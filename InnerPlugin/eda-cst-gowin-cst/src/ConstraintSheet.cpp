#include "ConstraintSheet.h"

#include <eda/api/Types.h>

#include "Platform.h"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <sstream>

namespace eda {
namespace cst {
namespace {

std::string Trim(const std::string& value) {
    const std::size_t first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const std::size_t last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::string ToUpper(std::string value) {
    for (char& c : value) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return value;
}

const target::PinEntry* FindPin(const target::PinDatabase& database, int pin) {
    for (const auto& entry : database.pins) {
        if (entry.pin == pin) return &entry;
    }
    return nullptr;
}

bool IsPinAvailable(const target::PinDatabase& database, int pin) {
    const auto* entry = FindPin(database, pin);
    return entry != nullptr && entry->available;
}

ConstraintError MakeError(std::string message, std::string field, int rowIndex) {
    ConstraintError error;
    error.message = std::move(message);
    error.field = std::move(field);
    error.rowIndex = rowIndex;
    return error;
}

} // namespace

PinBinding* ConstraintSheet::FindBinding(const std::string& portName, int bitIndex) {
    for (auto& binding : bindings) {
        if (binding.portName == portName && binding.bitIndex == bitIndex) return &binding;
    }
    return nullptr;
}

const PinBinding* ConstraintSheet::FindBinding(const std::string& portName, int bitIndex) const {
    for (const auto& binding : bindings) {
        if (binding.portName == portName && binding.bitIndex == bitIndex) return &binding;
    }
    return nullptr;
}

bool ConstraintSheet::IsPortBound(const std::string& portName, int bitIndex) const {
    const auto* binding = FindBinding(portName, bitIndex);
    return binding != nullptr && binding->packagePin > 0;
}

std::vector<const PinBinding*> ConstraintSheet::GetUnboundPorts() const {
    std::vector<const PinBinding*> result;
    for (const auto& binding : bindings) {
        if (binding.packagePin <= 0) result.push_back(&binding);
    }
    return result;
}

ConstraintValidator::ConstraintValidator(const target::PinDatabase& pinDb) : pinDb_(pinDb) {}

PortMap ConstraintValidator::ParsePortsFromJsonString(const std::string& jsonContent,
                                                      const std::string& topModule,
                                                      std::string& errorMessage) const {
    PortMap ports;
    if (jsonContent.empty()) {
        errorMessage = "Empty JSON content.";
        return ports;
    }
    Json root;
    try {
        root = Json::parse(jsonContent);
    } catch (const std::exception& parseError) {
        errorMessage = parseError.what();
        return ports;
    }
    if (!root.contains("modules") || !root["modules"].is_object()) {
        errorMessage = "Yosys JSON format error: missing 'modules' object.";
        return ports;
    }
    const Json& modules = root["modules"];
    if (!modules.contains(topModule) || !modules[topModule].is_object()) {
        errorMessage = "Top module '" + topModule + "' was not found in the Yosys JSON netlist.";
        return ports;
    }
    const Json& target = modules[topModule];
    if (!target.contains("ports") || !target["ports"].is_object()) {
        return ports;  // 无端口 — 空结果但不是错误
    }
    for (auto it = target["ports"].begin(); it != target["ports"].end(); ++it) {
        const Json& portInfo = it.value();
        PortInfo info;
        if (portInfo.contains("direction") && portInfo["direction"].is_string()) {
            info.direction = PortDirectionFromString(portInfo["direction"].get<std::string>());
        }
        if (portInfo.contains("bits") && portInfo["bits"].is_array()) {
            info.width = static_cast<int>(portInfo["bits"].size());
        }
        ports[it.key()] = info;
    }
    return ports;
}

PortMap ConstraintValidator::ExtractPortsFromYosysJson(const std::string& jsonPath,
                                                       const std::string& topModule,
                                                       std::string& errorMessage) const {
    std::ifstream input(jsonPath, std::ios::binary);
    if (!input) {
        errorMessage = "Cannot open Yosys JSON file: " + jsonPath;
        return {};
    }
    const std::string content((std::istreambuf_iterator<char>(input)),
                              std::istreambuf_iterator<char>());
    return ParsePortsFromJsonString(content, topModule, errorMessage);
}

std::vector<ConstraintError> ConstraintValidator::Validate(const ConstraintSheet& sheet,
                                                           const PortMap& ports) const {
    std::vector<ConstraintError> errors;

    for (std::size_t i = 0; i < sheet.bindings.size(); ++i) {
        const auto& binding = sheet.bindings[i];
        const auto portIt = ports.find(binding.portName);
        if (portIt == ports.end()) {
            errors.push_back(MakeError(
                "Port '" + binding.portName + "' not found in synthesized netlist.",
                "bindings[" + std::to_string(i) + "].port", static_cast<int>(i)));
            continue;
        }
        const int width = portIt->second.width;
        if (binding.packagePin <= 0) {
            errors.push_back(MakeError(
                "Port '" + binding.portName + "' is not assigned to a package pin.",
                "bindings[" + std::to_string(i) + "].package_pin", static_cast<int>(i)));
        }
        if (binding.bitIndex >= width || binding.bitIndex < 0) {
            errors.push_back(MakeError(
                "Bit index " + std::to_string(binding.bitIndex) + " out of range for port '" +
                    binding.portName + "' (width=" + std::to_string(width) + ").",
                "bindings[" + std::to_string(i) + "].bit", static_cast<int>(i)));
        }
    }

    std::map<int, std::vector<int>> pinUsage;
    for (std::size_t i = 0; i < sheet.bindings.size(); ++i) {
        if (sheet.bindings[i].packagePin > 0) {
            pinUsage[sheet.bindings[i].packagePin].push_back(static_cast<int>(i));
        }
    }
    for (const auto& [pin, indices] : pinUsage) {
        if (indices.size() > 1) {
            errors.push_back(MakeError(
                "Package pin " + std::to_string(pin) + " is assigned to " +
                    std::to_string(indices.size()) + " ports.",
                "bindings[" + std::to_string(indices[1]) + "].package_pin", indices[1]));
        }
    }

    for (std::size_t i = 0; i < sheet.bindings.size(); ++i) {
        const auto& binding = sheet.bindings[i];
        if (binding.packagePin <= 0) continue;
        if (binding.packagePin < 1 || binding.packagePin > 88) {
            errors.push_back(MakeError(
                "Pin " + std::to_string(binding.packagePin) + " is out of range (1-88) for QFN88.",
                "bindings[" + std::to_string(i) + "].package_pin", static_cast<int>(i)));
            continue;
        }
        if (!IsPinAvailable(pinDb_, binding.packagePin)) {
            const auto* pin = FindPin(pinDb_, binding.packagePin);
            const std::string reason =
                (pin != nullptr && !pin->reservedReason.empty()) ? pin->reservedReason
                                                                 : std::string("Unavailable");
            errors.push_back(MakeError(
                "Pin " + std::to_string(binding.packagePin) + " is reserved: " + reason,
                "bindings[" + std::to_string(i) + "].package_pin", static_cast<int>(i)));
        }
    }

    for (std::size_t i = 0; i < sheet.bindings.size(); ++i) {
        const auto& binding = sheet.bindings[i];
        if (binding.ioType.empty() || binding.packagePin <= 0) continue;
        const auto& known = pinDb_.ioTypes;
        if (std::find(known.begin(), known.end(), binding.ioType) == known.end()) {
            ConstraintError warning = MakeError(
                "Unknown IO_TYPE '" + binding.ioType + "' for port '" + binding.portName +
                    "'. Verify with device datasheet.",
                "bindings[" + std::to_string(i) + "].io_type", static_cast<int>(i));
            warning.severity = ConstraintError::Severity::Warning;
            errors.push_back(warning);
        }
    }

    std::map<std::string, int> boundBitCounts;
    for (const auto& binding : sheet.bindings) {
        if (binding.packagePin > 0) boundBitCounts[binding.portName]++;
    }
    for (const auto& [portName, info] : ports) {
        if (info.width > 1) {
            const int bound = boundBitCounts[portName];
            if (bound > 0 && bound < info.width) {
                errors.push_back(MakeError(
                    "Vector port '" + portName + "' has width " + std::to_string(info.width) +
                        " but only " + std::to_string(bound) + " bits are bound.",
                    "bindings", -1));
            }
        }
    }

    std::sort(errors.begin(), errors.end(),
              [](const ConstraintError& a, const ConstraintError& b) {
                  return static_cast<int>(a.severity) < static_cast<int>(b.severity);
              });
    return errors;
}

std::vector<ConstraintError> ConstraintValidator::ValidateBinding(
    const PinBinding& binding) const {
    std::vector<ConstraintError> errors;
    if (binding.portName.empty()) {
        errors.push_back(MakeError("Port name is empty.", "port", -1));
    }
    if (binding.packagePin > 0) {
        if (binding.packagePin < 1 || binding.packagePin > 88) {
            errors.push_back(MakeError(
                "Pin " + std::to_string(binding.packagePin) + " out of QFN88 range (1-88).",
                "package_pin", -1));
        } else if (!IsPinAvailable(pinDb_, binding.packagePin)) {
            errors.push_back(MakeError(
                "Pin " + std::to_string(binding.packagePin) + " is reserved.", "package_pin", -1));
        }
    }
    return errors;
}

CstGenerationResult CstGenerator::Generate(const ConstraintSheet& sheet) const {
    CstGenerationResult result;

    std::vector<std::pair<std::string, std::string>> entries;
    for (const auto& binding : sheet.bindings) {
        entries.emplace_back(
            binding.portName + "_" + std::to_string(binding.bitIndex),
            std::to_string(binding.packagePin) + "|" + binding.ioType + "|" + binding.drive + "|" +
                (binding.pullUp ? "1" : "0") + "|" + (binding.pullDown ? "1" : "0"));
    }
    std::sort(entries.begin(), entries.end());
    unsigned long hash = 5381;
    for (const auto& [key, value] : entries) {
        for (char c : key + "=" + value) {
            hash = ((hash << 5) + hash) + static_cast<unsigned long>(static_cast<unsigned char>(c));
        }
    }
    char hashBuffer[16];
    std::snprintf(hashBuffer, sizeof(hashBuffer), "%08lx", hash);
    result.bindingsHash = hashBuffer;

    std::ostringstream out;
    out << "// Tang Nano 9K pin constraints - generated by SigFlow\n";
    out << "// Profile: " << sheet.targetProfileId << "@" << sheet.targetProfileVersion
        << "  Device: " << sheet.device << "\n";
    out << "// Top module: " << sheet.topModule << "  Bindings hash: " << result.bindingsHash
        << "\n";
    out << "// Generated: " << sheet.generatedAt << "\n//\n";
    out << "// Unused pins can be assigned with:\n";
    out << "//   IO_LOC <pin> port=\"__UNUSED\";\n\n";

    std::vector<const PinBinding*> sorted;
    for (const auto& binding : sheet.bindings) {
        if (binding.packagePin > 0) sorted.push_back(&binding);
    }
    std::sort(sorted.begin(), sorted.end(), [](const PinBinding* a, const PinBinding* b) {
        if (a->portName != b->portName) return a->portName < b->portName;
        return a->bitIndex < b->bitIndex;
    });
    if (sorted.empty()) {
        result.errors.push_back(MakeError(
            "Cannot generate CST because no ports are assigned to package pins.", "bindings", -1));
        return result;
    }

    for (const auto* binding : sorted) {
        const std::string portRef = binding->bitIndex > 0
            ? binding->portName + "[" + std::to_string(binding->bitIndex) + "]"
            : binding->portName;
        if (!binding->comment.empty()) out << "// " << binding->comment << "\n";
        out << "IO_LOC \"" << portRef << "\" " << binding->packagePin << ";\n";
        std::string attributes;
        if (!binding->ioType.empty()) attributes += " IO_TYPE=" + binding->ioType;
        if (!binding->drive.empty()) attributes += " DRIVE=" + binding->drive;
        if (binding->pullUp) attributes += " PULL_MODE=UP";
        else if (binding->pullDown) attributes += " PULL_MODE=DOWN";
        if (!attributes.empty()) {
            out << "IO_PORT \"" << portRef << "\"" << attributes << ";\n";
        }
        out << "\n";
    }

    result.cstContent = out.str();
    result.success = true;
    return result;
}

std::vector<PinBinding> CstGenerator::ImportCst(const std::string& cstContent,
                                                std::vector<std::string>& unmanagedLines,
                                                std::string& errorMessage) const {
    (void)errorMessage;
    std::vector<PinBinding> bindings;
    unmanagedLines.clear();

    std::istringstream stream(cstContent);
    std::string line;
    while (std::getline(stream, line)) {
        const std::string trimmed = Trim(line);
        if (trimmed.empty() || trimmed.rfind("//", 0) == 0 || trimmed.rfind("#", 0) == 0) continue;

        if (trimmed.rfind("IO_LOC", 0) == 0) {
            const std::size_t q1 = trimmed.find('"');
            const std::size_t q2 = q1 == std::string::npos ? std::string::npos
                                                           : trimmed.find('"', q1 + 1);
            if (q1 == std::string::npos || q2 == std::string::npos) {
                unmanagedLines.push_back(line);
                continue;
            }
            const std::string portRef = trimmed.substr(q1 + 1, q2 - q1 - 1);
            std::string pinStr = trimmed.substr(q2 + 1);
            pinStr = Trim(pinStr.substr(0, pinStr.find(';')));
            try {
                PinBinding binding;
                binding.source = "import";
                binding.packagePin = std::stoi(pinStr);
                const std::size_t bracket = portRef.find('[');
                if (bracket != std::string::npos && portRef.find(']') != std::string::npos) {
                    binding.portName = portRef.substr(0, bracket);
                    binding.bitIndex =
                        std::stoi(portRef.substr(bracket + 1, portRef.find(']') - bracket - 1));
                } else {
                    binding.portName = portRef;
                }
                bindings.push_back(binding);
            } catch (const std::exception&) {
                unmanagedLines.push_back(line);
            }
            continue;
        }

        if (trimmed.rfind("IO_PORT", 0) == 0) {
            const std::size_t q1 = trimmed.find('"');
            const std::size_t q2 = q1 == std::string::npos ? std::string::npos
                                                           : trimmed.find('"', q1 + 1);
            if (q1 == std::string::npos || q2 == std::string::npos) {
                unmanagedLines.push_back(line);
                continue;
            }
            const std::string portRef = trimmed.substr(q1 + 1, q2 - q1 - 1);
            std::string attrs = trimmed.substr(q2 + 1);
            attrs = attrs.substr(0, attrs.find(';'));

            std::string portName = portRef;
            int bitIndex = 0;
            const std::size_t bracket = portRef.find('[');
            if (bracket != std::string::npos && portRef.find(']') != std::string::npos) {
                portName = portRef.substr(0, bracket);
                try {
                    bitIndex =
                        std::stoi(portRef.substr(bracket + 1, portRef.find(']') - bracket - 1));
                } catch (const std::exception&) {
                }
            }
            for (auto& binding : bindings) {
                if (binding.portName != portName || binding.bitIndex != bitIndex) continue;
                std::istringstream tokens(attrs);
                std::string token;
                while (tokens >> token) {
                    const std::size_t equals = token.find('=');
                    if (equals == std::string::npos) continue;
                    const std::string key = ToUpper(token.substr(0, equals));
                    const std::string value = token.substr(equals + 1);
                    if (key == "IO_TYPE") binding.ioType = value;
                    else if (key == "DRIVE") binding.drive = value;
                    else if (key == "PULL_MODE") {
                        if (ToUpper(value) == "UP") { binding.pullUp = true; binding.pullDown = false; }
                        else if (ToUpper(value) == "DOWN") { binding.pullDown = true; binding.pullUp = false; }
                    }
                }
                break;
            }
            continue;
        }

        unmanagedLines.push_back(line);
    }
    return bindings;
}

bool LoadConstraintSheet(const std::filesystem::path& filePath, ConstraintSheet& sheet,
                         std::string& errorMessage) {
    std::ifstream input(filePath, std::ios::binary);
    if (!input) {
        errorMessage = "Cannot open pin-bindings.json: " + filePath.string();
        return false;
    }
    const std::string content((std::istreambuf_iterator<char>(input)),
                              std::istreambuf_iterator<char>());
    Json root;
    try {
        root = Json::parse(content);
    } catch (const std::exception& parseError) {
        errorMessage = std::string("JSON parse error: ") + parseError.what();
        return false;
    }
    sheet.targetProfileId = root.value("target_profile", std::string());
    sheet.targetProfileVersion = root.value("profile_version", std::string());
    sheet.topModule = root.value("top_module", std::string());
    sheet.device = root.value("device", std::string());
    sheet.generatedAt = root.value("generated", std::string());
    sheet.bindings.clear();
    if (root.contains("bindings") && root["bindings"].is_array()) {
        for (const auto& element : root["bindings"]) {
            PinBinding binding;
            binding.portName = element.value("port", std::string());
            binding.bitIndex = element.value("bit", 0);
            binding.direction = PortDirectionFromString(element.value("direction", "input"));
            binding.packagePin = element.value("package_pin", -1);
            binding.ioType = element.value("io_type", std::string("LVCMOS33"));
            binding.drive = element.value("drive", std::string());
            binding.pullUp = element.value("pull_up", false);
            binding.pullDown = element.value("pull_down", false);
            binding.comment = element.value("comment", std::string());
            binding.source = element.value("source", std::string("gui"));
            sheet.bindings.push_back(std::move(binding));
        }
    }
    return true;
}

bool SaveConstraintSheet(const std::filesystem::path& filePath, const ConstraintSheet& sheet,
                         std::string& errorMessage) {
    std::error_code error;
    std::filesystem::create_directories(filePath.parent_path(), error);

    Json root;
    root["schema_version"] = "1.0";
    root["target_profile"] = sheet.targetProfileId;
    root["profile_version"] = sheet.targetProfileVersion;
    root["top_module"] = sheet.topModule;
    root["device"] = sheet.device;
    root["generated"] = sheet.generatedAt.empty() ? platform::UtcTimestamp() : sheet.generatedAt;

    Json bindings = Json::array();
    for (const auto& binding : sheet.bindings) {
        bindings.push_back(Json{{"port", binding.portName},
                                {"bit", binding.bitIndex},
                                {"direction", PortDirectionToString(binding.direction)},
                                {"package_pin", binding.packagePin},
                                {"io_type", binding.ioType},
                                {"drive", binding.drive},
                                {"pull_up", binding.pullUp},
                                {"pull_down", binding.pullDown},
                                {"comment", binding.comment},
                                {"source", binding.source}});
    }
    root["bindings"] = bindings;

    std::ofstream out(filePath, std::ios::trunc | std::ios::binary);
    if (!out) {
        errorMessage = "Cannot write to: " + filePath.string();
        return false;
    }
    out << root.dump(2) << "\n";
    if (!out) {
        errorMessage = "Write failed for: " + filePath.string();
        return false;
    }
    return true;
}

std::filesystem::path FindYosysJsonPath(const std::filesystem::path& projectRoot,
                                        const std::string& topModule) {
    const std::filesystem::path candidate = projectRoot / "yosys" / (topModule + ".json");
    std::error_code error;
    if (std::filesystem::is_regular_file(candidate, error)) return candidate;
    return {};
}

} // namespace cst
} // namespace eda
