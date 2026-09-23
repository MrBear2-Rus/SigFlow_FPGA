#include "GowinCst.h"

#include <algorithm>
#include <cctype>
#include <sstream>

namespace eda {
namespace cst {
namespace {

std::string Trim(std::string value) {
    const std::size_t first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const std::size_t last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

bool StartsWith(const std::string& value, const char* prefix) {
    return value.rfind(prefix, 0) == 0;
}

// 提取形如 "name" 的引号内标识符（跳过前导可选关键字）。
bool ExtractQuotedName(const std::string& text, std::string& name, std::size_t& after) {
    const std::size_t begin = text.find('"');
    if (begin == std::string::npos) return false;
    const std::size_t end = text.find('"', begin + 1);
    if (end == std::string::npos) return false;
    name = text.substr(begin + 1, end - begin - 1);
    after = end + 1;
    return !name.empty();
}

CstBinding* FindOrAdd(std::vector<CstBinding>& bindings, const std::string& name) {
    for (auto& binding : bindings) {
        if (binding.portName == name) return &binding;
    }
    CstBinding binding;
    binding.portName = name;
    bindings.push_back(binding);
    return &bindings.back();
}

} // namespace

std::string GowinCstCodec::Generate(const std::vector<CstBinding>& bindings) {
    std::ostringstream out;
    for (const auto& binding : bindings) {
        if (binding.packagePin < 0) continue;
        out << "IO_LOC \"" << binding.portName << "\" " << binding.packagePin << ";\n";
        out << "IO_PORT \"" << binding.portName << "\"";
        out << " IO_TYPE=" << (binding.ioType.empty() ? "LVCMOS33" : binding.ioType);
        if (!binding.drive.empty()) out << " DRIVE=" << binding.drive;
        if (binding.pullUp) out << " PULL_MODE=UP";
        else if (binding.pullDown) out << " PULL_MODE=DOWN";
        out << ";\n";
    }
    return out.str();
}

bool GowinCstCodec::Parse(const std::string& text, std::vector<CstBinding>& bindings,
                          std::vector<CstError>& errors) {
    bindings.clear();
    errors.clear();

    std::istringstream stream(text);
    std::string rawLine;
    int lineNumber = 0;
    while (std::getline(stream, rawLine)) {
        ++lineNumber;
        const std::string line = Trim(rawLine);
        if (line.empty() || StartsWith(line, "//") || StartsWith(line, "#")) continue;

        if (StartsWith(line, "IO_LOC")) {
            std::string name;
            std::size_t after = 0;
            if (!ExtractQuotedName(line, name, after)) {
                errors.push_back({"error", "IO_LOC is missing a quoted port name", lineNumber});
                continue;
            }
            const std::string rest = Trim(line.substr(after));
            const std::size_t semicolon = rest.find(';');
            const std::string pinText = Trim(rest.substr(0, semicolon));
            try {
                const int pin = std::stoi(pinText);
                FindOrAdd(bindings, name)->packagePin = pin;
            } catch (const std::exception&) {
                errors.push_back({"error", "IO_LOC pin number is not an integer: " + name, lineNumber});
            }
            continue;
        }

        if (StartsWith(line, "IO_PORT")) {
            std::string name;
            std::size_t after = 0;
            if (!ExtractQuotedName(line, name, after)) {
                errors.push_back({"error", "IO_PORT is missing a quoted port name", lineNumber});
                continue;
            }
            CstBinding* binding = FindOrAdd(bindings, name);
            std::string attributes = line.substr(after);
            std::istringstream attrs(attributes);
            std::string token;
            while (attrs >> token) {
                if (!token.empty() && token.back() == ';') token.pop_back();
                const std::size_t equals = token.find('=');
                if (equals == std::string::npos) continue;
                const std::string key = token.substr(0, equals);
                const std::string value = token.substr(equals + 1);
                if (key == "IO_TYPE") binding->ioType = value;
                else if (key == "DRIVE") binding->drive = value;
                else if (key == "PULL_MODE") {
                    binding->pullUp = (value == "UP");
                    binding->pullDown = (value == "DOWN");
                }
            }
            continue;
        }

        errors.push_back({"warning", "unrecognized CST statement: " + line, lineNumber});
    }

    for (const auto& error : errors) {
        if (error.IsError()) return false;
    }
    return true;
}

} // namespace cst
} // namespace eda
