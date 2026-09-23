#include "GowinPackService.h"

#include <eda/api/Types.h>

#include "Platform.h"
#include "eda-platform/Sha256.h"

#include <filesystem>
#include <fstream>
#include <system_error>

namespace eda {
namespace pack {
namespace {

constexpr std::uint64_t kMinimumBitstreamSizeBytes = 32;

std::string NormalizePath(const std::string& path) {
    std::error_code error;
    const std::filesystem::path normalized =
        std::filesystem::absolute(std::filesystem::path(path), error).lexically_normal();
    if (error) return path;
    return normalized.generic_string();
}

bool GetFileSize(const std::string& path, std::uint64_t& sizeBytes) {
    std::error_code error;
    const std::uintmax_t size = std::filesystem::file_size(path, error);
    if (error) return false;
    sizeBytes = static_cast<std::uint64_t>(size);
    return true;
}

bool FileExists(const std::string& path) {
    std::error_code error;
    return std::filesystem::is_regular_file(path, error);
}

Json ToJson(const PackReport& report) {
    Json root;
    root["schema_version"] = "1.0";
    root["tool"] = "gowin_pack";
    root["success"] = report.success;
    root["exit_code"] = report.exitCode;
    root["message"] = report.message;
    root["completed_at"] = report.completedAt;
    root["device"] = report.device;
    root["input"] = Json{{"pnr_json", report.inputPnrJsonPath},
                         {"size_bytes", report.inputPnrJsonSizeBytes},
                         {"sha256", report.inputPnrJsonSha256}};
    root["output"] = Json{{"fs", report.bitstreamPath},
                          {"size_bytes", report.bitstreamSizeBytes},
                          {"sha256", report.bitstreamSha256}};
    root["executable"] = Json{{"path", report.executablePath},
                              {"sha256", report.executableSha256}};
    return root;
}

} // namespace

bool PackService::ValidateInput(const std::string& pnrJsonPath, std::string& errorMessage) const {
    const std::string normalizedPath = NormalizePath(pnrJsonPath);
    if (normalizedPath.empty() || !FileExists(normalizedPath)) {
        errorMessage = "PnR JSON artifact does not exist: " + normalizedPath;
        return false;
    }
    std::uint64_t sizeBytes = 0;
    if (!GetFileSize(normalizedPath, sizeBytes) || sizeBytes == 0) {
        errorMessage = "PnR JSON artifact is empty or cannot be read: " + normalizedPath;
        return false;
    }
    return true;
}

bool PackService::Finalize(const PackRequest& request, int exitCode, PackReport& report) const {
    report = PackReport();
    report.exitCode = exitCode;
    report.completedAt = platform::UtcTimestamp();
    report.inputPnrJsonPath = NormalizePath(request.pnrJsonPath);
    report.bitstreamPath = NormalizePath(request.bitstreamPath);
    report.executablePath = NormalizePath(request.executablePath);
    report.device = request.device;

    if (FileExists(report.inputPnrJsonPath)) {
        GetFileSize(report.inputPnrJsonPath, report.inputPnrJsonSizeBytes);
        report.inputPnrJsonSha256 = platform::Sha256FileHex(report.inputPnrJsonPath);
    }
    if (FileExists(report.executablePath)) {
        report.executableSha256 = platform::Sha256FileHex(report.executablePath);
    }

    if (exitCode != 0) {
        report.message = "gowin_pack exited with code " + std::to_string(exitCode) + ".";
        return false;
    }
    if (!FileExists(report.bitstreamPath)) {
        report.message = "gowin_pack reported success but did not create the .fs artifact.";
        return false;
    }
    if (!GetFileSize(report.bitstreamPath, report.bitstreamSizeBytes) ||
        report.bitstreamSizeBytes < kMinimumBitstreamSizeBytes) {
        report.message = "The generated .fs artifact is too small or cannot be read.";
        return false;
    }
    report.bitstreamSha256 = platform::Sha256FileHex(report.bitstreamPath);
    if (report.inputPnrJsonSha256.empty() || report.executableSha256.empty() ||
        report.bitstreamSha256.empty()) {
        report.message = "Unable to calculate SHA-256 for a pack input or output artifact.";
        return false;
    }

    report.success = true;
    report.message = "Apicula .fs artifact is valid.";
    return true;
}

bool PackService::WriteManifest(const std::string& manifestPath, const PackReport& report,
                                std::string& errorMessage) const {
    const std::filesystem::path target(manifestPath);
    const std::filesystem::path temporary = std::filesystem::path(manifestPath).concat(".tmp");
    std::error_code error;
    std::filesystem::create_directories(target.parent_path(), error);
    {
        std::ofstream out(temporary, std::ios::trunc | std::ios::binary);
        if (!out) {
            errorMessage = "Unable to write pack manifest: " + manifestPath;
            return false;
        }
        out << ToJson(report).dump(2) << "\n";
        if (!out) {
            errorMessage = "Unable to write pack manifest: " + manifestPath;
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
        errorMessage = "Unable to replace pack manifest: " + manifestPath;
        std::error_code cleanupError;
        std::filesystem::remove(temporary, cleanupError);
        return false;
    }
    return true;
}

} // namespace pack
} // namespace eda
