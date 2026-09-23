#pragma once

#include "PinDatabaseStore.h"

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace eda {
namespace cst {

enum class PortDirection { Input, Output, InOut };

inline std::string PortDirectionToString(PortDirection direction) {
    switch (direction) {
        case PortDirection::Input:  return "input";
        case PortDirection::Output: return "output";
        case PortDirection::InOut:  return "inout";
    }
    return "unknown";
}

inline PortDirection PortDirectionFromString(const std::string& value) {
    if (value == "output" || value == "out" || value == "Out") return PortDirection::Output;
    if (value == "inout" || value == "InOut") return PortDirection::InOut;
    return PortDirection::Input;
}

struct PortInfo {
    PortDirection direction = PortDirection::Input;
    int width = 1;
};

using PortMap = std::map<std::string, PortInfo>;

struct PinBinding {
    std::string portName;
    int bitIndex = 0;
    PortDirection direction = PortDirection::Input;
    int packagePin = -1;
    std::string ioType = "LVCMOS33";
    std::string drive;
    bool pullUp = false;
    bool pullDown = false;
    std::string comment;
    std::string source;
};

struct ConstraintError {
    enum class Severity { Error, Warning };
    Severity severity = Severity::Error;
    std::string message;
    std::string field;
    int rowIndex = -1;
    bool IsError() const { return severity == Severity::Error; }
    bool IsWarning() const { return severity == Severity::Warning; }
};

struct ConstraintSheet {
    std::string targetProfileId;
    std::string targetProfileVersion;
    std::string topModule;
    std::string device;
    std::vector<PinBinding> bindings;
    std::string generatedAt;

    PinBinding* FindBinding(const std::string& portName, int bitIndex);
    const PinBinding* FindBinding(const std::string& portName, int bitIndex) const;
    bool IsPortBound(const std::string& portName, int bitIndex) const;
    std::vector<const PinBinding*> GetUnboundPorts() const;
};

struct CstGenerationResult {
    bool success = false;
    std::string cstContent;
    std::vector<ConstraintError> errors;
    std::string bindingsHash;
};

// 自 `main/fpga/FpgaConstraint` 移入并去 wx 化（nlohmann + std::filesystem）。
class ConstraintValidator {
public:
    explicit ConstraintValidator(const target::PinDatabase& pinDb);

    PortMap ParsePortsFromJsonString(const std::string& jsonContent,
                                     const std::string& topModule, std::string& errorMessage) const;
    PortMap ExtractPortsFromYosysJson(const std::string& jsonPath,
                                      const std::string& topModule, std::string& errorMessage) const;

    std::vector<ConstraintError> Validate(const ConstraintSheet& sheet, const PortMap& ports) const;
    std::vector<ConstraintError> ValidateBinding(const PinBinding& binding) const;

private:
    const target::PinDatabase& pinDb_;
};

class CstGenerator {
public:
    CstGenerationResult Generate(const ConstraintSheet& sheet) const;
    std::vector<PinBinding> ImportCst(const std::string& cstContent,
                                      std::vector<std::string>& unmanagedLines,
                                      std::string& errorMessage) const;
};

bool LoadConstraintSheet(const std::filesystem::path& filePath, ConstraintSheet& sheet,
                         std::string& errorMessage);
bool SaveConstraintSheet(const std::filesystem::path& filePath, const ConstraintSheet& sheet,
                         std::string& errorMessage);
std::filesystem::path FindYosysJsonPath(const std::filesystem::path& projectRoot,
                                        const std::string& topModule);

} // namespace cst
} // namespace eda
