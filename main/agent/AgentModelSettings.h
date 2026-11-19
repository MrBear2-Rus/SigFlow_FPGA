#pragma once

// Host-side model credential store for the Agent sidecar.
//
// Only the host reads/writes this file; the sidecar receives the resolved
// values once through the stdin bootstrap.  On Windows the payload is
// encrypted with the current user's DPAPI key; no plaintext fallback exists.
// On other platforms only explicit environment variables are used.

#include <filesystem>
#include <string>

namespace agent {

enum class ModelConfigSource {
    None = 0,
    Environment,
    Stored,
    EmbeddedDev,
};

/// One resolved model configuration plus where it came from.
struct ModelSettings
{
    std::string apiKey;
    std::string model;
    std::string baseUrl;
    ModelConfigSource source = ModelConfigSource::None;
    /// Fixed diagnostic code when a present source could not be used.
    std::string errorCode;

    bool configured() const { return !apiKey.empty(); }
};

/// Stable identifier for logs/tests ("none", "environment", "stored",
/// "embedded-dev").
const char* ModelConfigSourceId(ModelConfigSource source);

class AgentModelSettings final
{
public:
    /// Resolve in this order:
    ///   1. any explicit model environment variable -> environment (whole set)
    ///   2. DPAPI store
    ///   3. compile-time embedded development config
    ///   4. none
    static ModelSettings Load(const std::filesystem::path& appDataRoot);

    /// Validate key/model/base URL using the same rules as the Python sidecar.
    /// Error codes: invalid_api_key, invalid_model, invalid_base_url.
    static bool Validate(const ModelSettings& settings, std::string* errorCode);

    /// Encrypt and atomically replace the per-user store.
    /// Error codes: unsupported_platform, invalid_*, key_store_unavailable,
    /// key_store_write_failed.
    static bool Save(const std::filesystem::path& appDataRoot, const ModelSettings& settings,
                     std::string* errorCode);

    /// Remove the stored credential.  Missing file is treated as success.
    static bool Clear(const std::filesystem::path& appDataRoot, std::string* errorCode);

    /// True when this platform can persist a credential (Windows DPAPI).
    static bool Supported();

    /// `<appDataRoot>/sigflow-edu-agent/model.credentials.json`
    static std::filesystem::path StorePath(const std::filesystem::path& appDataRoot);
};

}  // namespace agent
