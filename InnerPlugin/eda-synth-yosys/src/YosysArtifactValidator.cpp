#include "YosysArtifactValidator.h"

#include <eda/api/Types.h>

#include "eda-platform/Sha256.h"

#include "Platform.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <system_error>

namespace eda {
namespace synth {
namespace {

constexpr const char* kYosysNetlistArtifactId = "yosys-netlist-json";

std::string NowUtc() { return platform::UtcTimestamp(); }

bool Fail(NetlistArtifactReport& report, ArtifactValidationStatus status,
          const std::string& message, const std::string& failedField = {}) {
    report.status = status;
    report.message = message;
    report.failedField = failedField;
    return false;
}

void InitializeReport(const std::string& jsonPath, const std::string& expectedTopModule,
                      NetlistArtifactReport& report) {
    report = NetlistArtifactReport();
    report.artifactId = kYosysNetlistArtifactId;
    report.expectedTopModule = expectedTopModule;
    report.validatedAt = NowUtc();

    std::error_code error;
    report.path = std::filesystem::absolute(std::filesystem::path(jsonPath), error)
                      .lexically_normal()
                      .generic_string();
    if (error) report.path = jsonPath;
}

Json ToJson(const NetlistArtifactReport& report) {
    Json root;
    root["schema_version"] = "1.0";
    root["artifact_id"] = report.artifactId;
    root["status"] = ToString(report.status);
    root["path"] = report.path;
    root["expected_top_module"] = report.expectedTopModule;
    root["failed_field"] = report.failedField;
    root["message"] = report.message;
    root["validated_at"] = report.validatedAt;
    root["size_bytes"] = report.sizeBytes;
    root["sha256"] = report.sha256;
    root["summary"] = Json{{"ports", report.portCount},
                           {"cells", report.cellCount},
                           {"netnames", report.netnameCount}};
    return root;
}

bool WriteJsonAtomically(const std::string& path, const Json& value, std::string& errorMessage) {
    const std::filesystem::path target(path);
    const std::filesystem::path temporary = std::filesystem::path(path).concat(".tmp");
    std::error_code error;
    std::filesystem::create_directories(target.parent_path(), error);
    {
        std::ofstream out(temporary, std::ios::trunc | std::ios::binary);
        if (!out) {
            errorMessage = "Unable to write artifact manifest: " + path;
            return false;
        }
        out << value.dump(2) << "\n";
        if (!out) {
            errorMessage = "Unable to write artifact manifest: " + path;
            return false;
        }
    }
    std::filesystem::rename(temporary, target, error);
    if (error) {
        std::error_code removeError;
        std::filesystem::remove(target, removeError);
        error.clear();
        std::filesystem::rename(temporary, target, error);
    }
    if (error) {
        errorMessage = "Unable to replace artifact manifest: " + path;
        std::error_code cleanupError;
        std::filesystem::remove(temporary, cleanupError);
        return false;
    }
    return true;
}

} // namespace

std::string ToString(ArtifactValidationStatus status) {
    switch (status) {
        case ArtifactValidationStatus::NotValidated:         return "NotValidated";
        case ArtifactValidationStatus::Valid:                return "Valid";
        case ArtifactValidationStatus::MissingFile:          return "MissingFile";
        case ArtifactValidationStatus::TooSmall:             return "TooSmall";
        case ArtifactValidationStatus::ReadFailed:           return "ReadFailed";
        case ArtifactValidationStatus::InvalidJson:          return "InvalidJson";
        case ArtifactValidationStatus::MissingModules:       return "MissingModules";
        case ArtifactValidationStatus::MissingTopModule:     return "MissingTopModule";
        case ArtifactValidationStatus::MissingRequiredField: return "MissingRequiredField";
        case ArtifactValidationStatus::HashFailed:           return "HashFailed";
    }
    return "Unknown";
}

ArtifactValidator::ArtifactValidator(ArtifactValidationOptions options) : options_(options) {}

bool ArtifactValidator::ValidateYosysJson(const std::string& jsonPath,
                                          const std::string& expectedTopModule,
                                          NetlistArtifactReport& report) const {
    InitializeReport(jsonPath, expectedTopModule, report);
    if (expectedTopModule.empty()) {
        return Fail(report, ArtifactValidationStatus::MissingTopModule,
                    "Expected top module name is empty.");
    }

    std::error_code sizeError;
    const std::uintmax_t fileSize = std::filesystem::file_size(report.path, sizeError);
    if (sizeError) {
        if (!std::filesystem::exists(report.path, sizeError)) {
            return Fail(report, ArtifactValidationStatus::MissingFile,
                        "Yosys JSON artifact does not exist.");
        }
        return Fail(report, ArtifactValidationStatus::ReadFailed,
                    "Unable to obtain Yosys JSON artifact size.");
    }
    report.sizeBytes = static_cast<std::uint64_t>(fileSize);
    if (report.sizeBytes < options_.minimumFileSizeBytes) {
        return Fail(report, ArtifactValidationStatus::TooSmall,
                    "Yosys JSON artifact is only " + std::to_string(report.sizeBytes) +
                        " bytes; minimum is " +
                        std::to_string(options_.minimumFileSizeBytes) + " bytes.");
    }

    std::ifstream input(report.path, std::ios::binary);
    if (!input) {
        return Fail(report, ArtifactValidationStatus::ReadFailed,
                    "Unable to read Yosys JSON artifact.");
    }
    const std::string content((std::istreambuf_iterator<char>(input)),
                              std::istreambuf_iterator<char>());
    if (content.empty()) {
        return Fail(report, ArtifactValidationStatus::InvalidJson,
                    "Yosys JSON artifact is empty.");
    }

    Json root;
    try {
        root = Json::parse(content);
    } catch (const std::exception& parseError) {
        return Fail(report, ArtifactValidationStatus::InvalidJson,
                    std::string("Unable to parse Yosys JSON artifact: ") + parseError.what());
    }
    if (!root.is_object() || !root.contains("modules") || !root["modules"].is_object()) {
        return Fail(report, ArtifactValidationStatus::MissingModules,
                    "Yosys JSON artifact must contain an object field 'modules'.", "modules");
    }

    const Json& modules = root["modules"];
    if (!modules.contains(expectedTopModule) || !modules[expectedTopModule].is_object()) {
        return Fail(report, ArtifactValidationStatus::MissingTopModule,
                    "Expected top module '" + expectedTopModule +
                        "' was not found in the Yosys JSON artifact.",
                    "modules." + expectedTopModule);
    }

    const Json& topModule = modules[expectedTopModule];
    for (const char* requiredField : {"ports", "cells", "netnames"}) {
        if (!topModule.contains(requiredField) || !topModule[requiredField].is_object()) {
            return Fail(report, ArtifactValidationStatus::MissingRequiredField,
                        "Yosys JSON top module '" + expectedTopModule +
                            "' is missing required object field '" + requiredField + "'.",
                        "modules." + expectedTopModule + "." + requiredField);
        }
    }

    report.portCount = topModule["ports"].size();
    report.cellCount = topModule["cells"].size();
    report.netnameCount = topModule["netnames"].size();
    report.sha256 = platform::Sha256FileHex(report.path);
    if (report.sha256.empty()) {
        return Fail(report, ArtifactValidationStatus::HashFailed,
                    "Unable to calculate SHA-256 for Yosys JSON artifact.");
    }

    report.status = ArtifactValidationStatus::Valid;
    report.message = "Yosys JSON artifact is valid.";
    return true;
}

bool ArtifactValidator::WriteManifest(const std::string& manifestPath,
                                      const NetlistArtifactReport& report,
                                      std::string& errorMessage) const {
    return WriteJsonAtomically(manifestPath, ToJson(report), errorMessage);
}

} // namespace synth
} // namespace eda
