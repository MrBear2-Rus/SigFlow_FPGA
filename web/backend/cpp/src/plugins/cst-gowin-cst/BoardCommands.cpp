#include "BoardCommands.h"
#include "ConstraintSheet.h"
#include "CstFileValidator.h"
#include "GowinCst.h"
#include "eda-core/JobPaths.h"
#include "eda-platform/Platform.h"
#include "plugins/target-tangnano9k/PinDatabaseStore.h"
#include "plugins/target-tangnano9k/TargetProfileStore.h"
#include <filesystem>
#include <fstream>
#include <map>
#include <regex>
#include <sstream>
#include <set>
#include <stdexcept>

namespace fs = std::filesystem;
namespace eda::web {
namespace {
std::string ReadFile(const fs::path& path, std::uintmax_t limit) {
    std::error_code error;
    if (!fs::is_regular_file(path, error) || error || fs::file_size(path, error) > limit || error)
        throw std::runtime_error("input missing, not a regular file, or too large: " +
                                 path.string());
    std::ifstream input(path, std::ios::binary);
    std::string data((std::istreambuf_iterator<char>(input)), {});
    if (input.bad() || !input.is_open())
        throw std::runtime_error("cannot read: " + path.string());
    return data;
}
bool Identifier(const std::string& id) {
    if (id.empty() || id.size() > 128 || id == "." || id == "..")
        return false;
    for (unsigned char c : id)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '-' || c == '_' || c == '.'))
            return false;
    return true;
}
Json Errors(const std::vector<cst::ConstraintError>& errors) {
    Json out = Json::array();
    for (const auto& error : errors)
        out.push_back({{"severity", error.IsError() ? "error" : "warning"},
                       {"message", error.message},
                       {"field", error.field},
                       {"row", error.rowIndex}});
    return out;
}
bool Valid(const Json& diagnostics) {
    for (const auto& error : diagnostics)
        if (error.at("severity") == "error")
            return false;
    return true;
}
target::PinDatabase LoadPins(const std::string& file) {
    target::PinDatabase pins;
    std::string error;
    if (!target::PinDatabaseStore::Parse(ReadFile(file, 2 * 1024 * 1024), pins, error))
        throw std::runtime_error(error);
    return pins;
}
}  // namespace
int BoardCommand(int argc, char** argv, const std::function<void(const Json&)>& emit,
                 const std::string& defaultDataDir) {
    try {
        if (argc < 3)
            throw std::invalid_argument("missing subcommand");
        const std::string command = argv[1], action = argv[2];
        std::set<std::string> allowed;
        if (command == "target" && action == "show")
            allowed = {"--id", "--data-dir"};
        else if (command == "validate" && action == "cst")
            allowed = {"--cst", "--pins"};
        else if (command == "validate" && action == "pins")
            allowed = {"--design", "--pins", "--netlist"};
        else
            throw std::invalid_argument("unknown subcommand");
        std::map<std::string, std::string> options;
        for (int i = 3; i < argc; ++i) {
            std::string option = argv[i];
            if (!allowed.count(option) || options.count(option) || i + 1 >= argc ||
                std::string(argv[i + 1]).starts_with("--") || std::string(argv[i + 1]).empty())
                throw std::invalid_argument("invalid or duplicate option: " + option);
            options[option] = argv[++i];
        }
        const auto required = [&](const char* name) -> std::string {
            if (!options.count(name))
                throw std::invalid_argument(std::string("missing option: ") + name);
            return options.at(name);
        };
        Json data;
        if (command == "target") {
            auto id = required("--id");
            if (!Identifier(id))
                throw std::invalid_argument("invalid profile id");
            auto configured = platform::EnvUtf8("SIGFLOW_DATA_DIR");
            fs::path dataDir = options.count("--data-dir")
                                   ? options.at("--data-dir")
                                   : (!configured.empty() ? configured : defaultDataDir);
            auto dir = fs::absolute(dataDir) / "target-profiles";
            auto path = CheckedPath(dir, id + ".json", true);
            auto content = ReadFile(path, 2 * 1024 * 1024);
            target::TargetProfileStore store;
            TargetProfile profile;
            std::string error;
            if (!store.Parse(content, profile, error) || profile.id != id)
                throw std::runtime_error(error.empty() ? "target profile id mismatch" : error);
            data = Json::parse(content);
        } else if (action == "cst") {
            const auto cstFile = required("--cst");
            auto pins = LoadPins(required("--pins"));
            auto content = ReadFile(cstFile, 100 * 1024);
            auto result = cst::CstFileValidator::Validate(cstFile);
            Json diagnostics = Json::array();
            for (const auto& item : result.lineErrors)
                diagnostics.push_back({{"severity", "error"},
                                       {"message", item.reason},
                                       {"line", item.line - 1},
                                       {"content", item.content}});
            if (!result.valid && diagnostics.empty())
                diagnostics.push_back({{"severity", "error"},
                                       {"message", result.errorSummary},
                                       {"line", -1},
                                       {"content", ""}});
            if (result.valid) {
                std::vector<cst::CstBinding> bindings;
                std::vector<cst::CstError> parseErrors;
                cst::GowinCstCodec::Parse(content, bindings, parseErrors);
                std::map<int, std::string> used;
                std::set<std::string> locPorts;
                std::istringstream lines(content);
                std::string line;
                int lineNumber = 0;
                const std::regex loc(R"rx(^\s*IO_LOC\s+"([^"]+)"\s+\d+\s*;)rx");
                while (std::getline(lines, line)) {
                    ++lineNumber;
                    std::smatch match;
                    if (std::regex_search(line, match, loc) &&
                        !locPorts.insert(match[1].str()).second)
                        diagnostics.push_back({{"severity", "error"},
                                               {"message", "port assigned more than once"},
                                               {"line", lineNumber - 1},
                                               {"content", match[1].str()}});
                }
                for (const auto& binding : bindings) {
                    if (binding.packagePin <= 0)
                        continue;
                    cst::PinBinding check;
                    check.portName = binding.portName;
                    check.packagePin = binding.packagePin;
                    for (const auto& error : cst::ConstraintValidator(pins).ValidateBinding(check))
                        diagnostics.push_back({{"severity", "error"},
                                               {"message", error.message},
                                               {"line", -1},
                                               {"content", binding.portName}});
                    if (!used.emplace(binding.packagePin, binding.portName).second)
                        diagnostics.push_back({{"severity", "error"},
                                               {"message", "package pin assigned more than once"},
                                               {"line", -1},
                                               {"content", binding.portName}});
                }
            }
            data = {{"valid", Valid(diagnostics)},
                    {"diagnostics", diagnostics},
                    {"io_loc_count", result.ioLocCount},
                    {"io_port_count", result.ioPortCount}};
        } else {
            const auto design = required("--design");
            const auto netlist = required("--netlist");
            auto pins = LoadPins(required("--pins"));
            auto designJson = Json::parse(ReadFile(design, 2 * 1024 * 1024));
            if (!designJson.is_object() || !designJson.contains("bindings") ||
                !designJson.at("bindings").is_array() || !designJson.contains("top_module") ||
                !designJson.at("top_module").is_string() ||
                designJson.at("top_module").get<std::string>().empty())
                throw std::invalid_argument("design requires top_module and bindings array");
            cst::ConstraintSheet sheet;
            std::string error;
            if (!cst::LoadConstraintSheet(design, sheet, error))
                throw std::runtime_error(error);
            if (!sheet.device.empty() && sheet.device != pins.device)
                throw std::invalid_argument("design device does not match pin database");
            auto ports = cst::ConstraintValidator(pins).ParsePortsFromJsonString(
                ReadFile(netlist, 32 * 1024 * 1024), sheet.topModule, error);
            if (!error.empty())
                throw std::invalid_argument(error);
            auto diagnostics = Errors(cst::ConstraintValidator(pins).Validate(sheet, ports));
            std::set<std::pair<std::string, int>> boundBits;
            for (std::size_t i = 0; i < sheet.bindings.size(); ++i) {
                const auto& binding = sheet.bindings[i];
                if (!boundBits.emplace(binding.portName, binding.bitIndex).second)
                    diagnostics.push_back({{"severity", "error"},
                                           {"message", "port bit assigned more than once"},
                                           {"field", "bindings[" + std::to_string(i) + "]"},
                                           {"row", i}});
            }
            data = {{"valid", Valid(diagnostics)},
                    {"diagnostics", diagnostics},
                    {"top_module", sheet.topModule}};
        }
        emit(Json{{"type", "result"}, {"ok", true}, {"data", data}});
        return 0;
    } catch (const std::exception& error) {
        const bool invalid = dynamic_cast<const std::invalid_argument*>(&error) != nullptr;
        emit(Json{{"type", "diagnostic"},
                  {"severity", "error"},
                  {"code", invalid ? "INVALID_ARGUMENT" : "EXECUTION_FAILED"},
                  {"message", error.what()}});
        return invalid ? 2 : 4;
    }
}
}  // namespace eda::web
