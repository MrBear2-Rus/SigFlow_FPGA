#pragma once

#include <cstdint>
#include <string>

namespace eda {
namespace pack {

struct PackRequest {
    std::string pnrJsonPath;
    std::string bitstreamPath;
    std::string executablePath;
    std::string device;
};

struct PackReport {
    bool success = false;
    int exitCode = -1;
    std::string message;
    std::string completedAt;

    std::string inputPnrJsonPath;
    std::uint64_t inputPnrJsonSizeBytes = 0;
    std::string inputPnrJsonSha256;

    std::string bitstreamPath;
    std::uint64_t bitstreamSizeBytes = 0;
    std::string bitstreamSha256;

    std::string executablePath;
    std::string executableSha256;
    std::string device;
};

// 自 `main/fpga/FpgaPackService` 移入并去 wx 化。
class PackService {
public:
    bool ValidateInput(const std::string& pnrJsonPath, std::string& errorMessage) const;
    bool Finalize(const PackRequest& request, int exitCode, PackReport& report) const;
    bool WriteManifest(const std::string& manifestPath, const PackReport& report,
                       std::string& errorMessage) const;
};

} // namespace pack
} // namespace eda
