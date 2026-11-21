// Agent model credential store smoke test.
//
// Verifies the host-side validation rules, the Windows DPAPI round-trip (the
// encrypted file never contains the key), environment precedence, corrupt
// store handling and clear semantics.  The test target does not define
// SIGFLOW_EMBED_LOCAL_MODEL, so the embedded development fallback is absent.

#include "agent/AgentModelSettings.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

#if defined(_WIN32)
#include <cstdlib>
static void SetEnv(const char* name, const char* value) { _putenv_s(name, value); }
static void UnsetEnv(const char* name) { _putenv_s(name, ""); }
#else
static void SetEnv(const char* name, const char* value) { setenv(name, value, 1); }
static void UnsetEnv(const char* name) { unsetenv(name); }
#endif

namespace {

int g_failures = 0;

void Check(bool ok, const char* message) {
    if (ok) {
        std::cout << "  ok: " << message << "\n";
    } else {
        ++g_failures;
        std::cout << "  FAIL: " << message << "\n";
    }
}

void ClearModelEnvironment() {
    UnsetEnv("SIGFLOW_EDU_MODEL_PROVIDER");
    UnsetEnv("SIGFLOW_EDU_MODEL_API_KEY");
    UnsetEnv("SIGFLOW_EDU_MODEL_BASE_URL");
    UnsetEnv("SIGFLOW_EDU_MODEL_NAME");
    UnsetEnv("DEEPSEEK_API_KEY");
    UnsetEnv("DEEPSEEK_MODEL");
    UnsetEnv("DEEPSEEK_BASE_URL");
    UnsetEnv("SIGFLOW_EDU_LOCAL_MODEL_DISABLE");
    UnsetEnv("SIGFLOW_EDU_LOCAL_MODEL_CONFIG");
}

std::string ReadFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(input)),
                       std::istreambuf_iterator<char>());
}

agent::ModelSettings GoodSettings(const std::string& apiKey = "sk-test-0123456789abcdef",
                                  const std::string& model = "deepseek-chat") {
    agent::ModelSettings settings;
    settings.apiKey = apiKey;
    settings.model = model;
    settings.baseUrl = "https://api.deepseek.com";
    return settings;
}

}  // namespace

int main() {
    ClearModelEnvironment();
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        ("sigflow-model-settings-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root, ec);

    std::cout << "validation\n";
    std::string error;
    Check(agent::AgentModelSettings::Validate(GoodSettings(), &error), "good config validates");
    Check(error.empty(), "good config leaves no error code");
    Check(!agent::AgentModelSettings::Validate(GoodSettings(""), &error) &&
              error == "invalid_api_key",
          "empty key is rejected");
    Check(!agent::AgentModelSettings::Validate(GoodSettings("sk-good", "bad\nmodel"), &error) &&
              error == "invalid_model",
          "newline in model is rejected");
    agent::ModelSettings badUrl = GoodSettings();
    badUrl.baseUrl = "https://example.com";
    Check(!agent::AgentModelSettings::Validate(badUrl, &error) && error == "invalid_base_url",
          "non-DeepSeek endpoint is rejected");

    std::cout << "empty store\n";
    agent::ModelSettings loaded = agent::AgentModelSettings::Load(root);
    Check(!loaded.configured(), "no environment/store/embedded leaves an empty config");
    Check(loaded.source == agent::ModelConfigSource::None, "empty config reports source none");

#if defined(_WIN32)
    std::cout << "dpapi round-trip\n";
    const std::string firstKey = "sk-first-0123456789abcdef";
    agent::ModelSettings first = GoodSettings(firstKey, "deepseek-chat");
    Check(agent::AgentModelSettings::Save(root, first, &error), "save succeeds");
    const std::filesystem::path store = agent::AgentModelSettings::StorePath(root);
    Check(std::filesystem::exists(store), "store file exists");
    const std::string raw = ReadFile(store);
    Check(!raw.empty() && raw.find(firstKey) == std::string::npos,
          "encrypted store does not contain the plaintext key");
    Check(raw.find("api_key") == std::string::npos,
          "encrypted store does not contain the plaintext field name");

    loaded = agent::AgentModelSettings::Load(root);
    Check(loaded.configured() && loaded.apiKey == firstKey && loaded.model == "deepseek-chat" &&
              loaded.baseUrl == "https://api.deepseek.com",
          "load returns the saved values");
    Check(loaded.source == agent::ModelConfigSource::Stored, "load reports source stored");

    const std::string secondKey = "sk-second-0123456789abcdef";
    Check(agent::AgentModelSettings::Save(root, GoodSettings(secondKey, "deepseek-reasoner"),
                                         &error),
          "second save replaces the store");
    loaded = agent::AgentModelSettings::Load(root);
    Check(loaded.apiKey == secondKey && loaded.model == "deepseek-reasoner",
          "load returns the newest values");
    const std::string rawSecond = ReadFile(store);
    Check(rawSecond.find(secondKey) == std::string::npos && rawSecond.find(firstKey) == std::string::npos,
          "neither key appears in the store after replacement");

    std::cout << "corrupt store\n";
    {
        std::ofstream out(store, std::ios::binary | std::ios::trunc);
        out << "not a credential envelope";
    }
    loaded = agent::AgentModelSettings::Load(root);
    Check(!loaded.configured(), "corrupt store yields no credential");
    Check(loaded.source == agent::ModelConfigSource::Stored &&
              loaded.errorCode == "key_store_invalid",
          "corrupt store reports key_store_invalid");
    Check(agent::AgentModelSettings::Save(root, GoodSettings(firstKey, "deepseek-chat"), &error),
          "save recovers after a corrupt store");
    Check(agent::AgentModelSettings::Load(root).apiKey == firstKey,
          "recovered store loads the key");

    std::cout << "environment precedence\n";
    SetEnv("SIGFLOW_EDU_MODEL_API_KEY", "sk-env-0123456789abcdef");
    SetEnv("SIGFLOW_EDU_MODEL_NAME", "env-model");
    SetEnv("SIGFLOW_EDU_MODEL_BASE_URL", "https://api.deepseek.com");
    loaded = agent::AgentModelSettings::Load(root);
    Check(loaded.source == agent::ModelConfigSource::Environment,
          "environment wins over the stored config");
    Check(loaded.apiKey == "sk-env-0123456789abcdef" && loaded.model == "env-model",
          "environment values are used as a whole");
    ClearModelEnvironment();

    std::cout << "clear\n";
    Check(agent::AgentModelSettings::Clear(root, &error), "clear succeeds");
    Check(!std::filesystem::exists(store), "clear removes the store file");
    loaded = agent::AgentModelSettings::Load(root);
    Check(!loaded.configured() && loaded.source == agent::ModelConfigSource::None,
          "load after clear is empty");
#else
    std::cout << "unsupported platform\n";
    Check(!agent::AgentModelSettings::Save(root, GoodSettings(), &error) &&
              error == "unsupported_platform",
          "save reports unsupported_platform");
    Check(!agent::AgentModelSettings::Clear(root, &error) && error == "unsupported_platform",
          "clear reports unsupported_platform");
#endif

    ClearModelEnvironment();
    std::filesystem::remove_all(root, ec);
    if (g_failures != 0) {
        std::cout << g_failures << " failure(s)\n";
        return 1;
    }
    std::cout << "all checks passed\n";
    return 0;
}
