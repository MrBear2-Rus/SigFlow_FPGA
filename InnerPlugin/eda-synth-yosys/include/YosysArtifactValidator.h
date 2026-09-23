#pragma once

#include <cstdint>
#include <string>

namespace eda {
namespace synth {

enum class ArtifactValidationStatus {
    NotValidated,
    Valid,
    MissingFile,
    TooSmall,
    ReadFailed,
    InvalidJson,
    MissingModules,
    MissingTopModule,
    MissingRequiredField,
    HashFailed,
};

struct ArtifactValidationOptions {
    std::uint64_t minimumFileSizeBytes = 32;
};

struct NetlistArtifactReport {
    ArtifactValidationStatus status = ArtifactValidationStatus::NotValidated;

    std::string artifactId;
    std::string path;
    std::string expectedTopModule;
    std::string failedField;
    std::string message;
    std::string validatedAt;

    std::uint64_t sizeBytes = 0;
    std::uint64_t portCount = 0;
    std::uint64_t cellCount = 0;
    std::uint64_t netnameCount = 0;

    std::string sha256;
};

std::string ToString(ArtifactValidationStatus status);

// 从 `main/fpga/ArtifactValidator` 移入并去 wx 化（std::filesystem + nlohmann + eda_platform Sha256）。
class ArtifactValidator {
public:
    explicit ArtifactValidator(ArtifactValidationOptions options = {});

    bool ValidateYosysJson(const std::string& jsonPath, const std::string& expectedTopModule,
                           NetlistArtifactReport& report) const;
    bool WriteManifest(const std::string& manifestPath, const NetlistArtifactReport& report,
                       std::string& errorMessage) const;

private:
    ArtifactValidationOptions options_;
};

} // namespace synth
} // namespace eda
