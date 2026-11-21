#include "agent/AgentModelSettings.h"

#include <json/json.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <vector>

#if defined(SIGFLOW_EMBED_LOCAL_MODEL)
#include "LocalModelConfig.h"
#endif

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincrypt.h>
#endif

namespace agent {
namespace {

constexpr const char* kStoreSchema = "sigflow.agent.model.dpapi.v1";
constexpr const char* kDefaultBaseUrl = "https://api.deepseek.com";
constexpr const char* kDefaultModel = "deepseek-chat";
constexpr std::size_t kMaxStoreBytes = 16 * 1024;

bool EnvPresent(const char* name) { return std::getenv(name) != nullptr; }

std::string EnvValue(const char* name) {
    const char* value = std::getenv(name);
    return value == nullptr ? std::string() : std::string(value);
}

std::string FirstEnvValue(std::initializer_list<const char*> names) {
    for (const char* name : names) {
        const std::string value = EnvValue(name);
        if (!value.empty()) return value;
    }
    return std::string();
}

bool PrintableAscii(const std::string& value, std::size_t maxLength) {
    if (value.empty() || value.size() > maxLength) return false;
    for (const unsigned char c : value) {
        if (c < 33 || c > 126) return false;
    }
    return true;
}

bool ParseJson(const std::string& text, Json::Value* out) {
    Json::CharReaderBuilder builder;
    std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    std::string errors;
    return reader->parse(text.data(), text.data() + text.size(), out, &errors);
}

std::string WriteJson(const Json::Value& value) {
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    return Json::writeString(builder, value);
}

// ---- base64 (kept local so this file stays independent of wx) ------------

constexpr const char kBase64Alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string Base64Encode(const unsigned char* data, std::size_t length) {
    std::string result;
    result.reserve(((length + 2) / 3) * 4);
    for (std::size_t i = 0; i < length; i += 3) {
        const unsigned int a = data[i];
        const unsigned int b = i + 1 < length ? data[i + 1] : 0;
        const unsigned int c = i + 2 < length ? data[i + 2] : 0;
        result.push_back(kBase64Alphabet[a >> 2]);
        result.push_back(kBase64Alphabet[((a & 0x03) << 4) | (b >> 4)]);
        result.push_back(i + 1 < length ? kBase64Alphabet[((b & 0x0F) << 2) | (c >> 6)] : '=');
        result.push_back(i + 2 < length ? kBase64Alphabet[c & 0x3F] : '=');
    }
    return result;
}

int DecodeBase64Char(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

bool Base64Decode(const std::string& text, std::vector<unsigned char>* out) {
    if (text.empty() || text.size() % 4 != 0 || text.size() > 65536) return false;
    out->clear();
    out->reserve(text.size() / 4 * 3);
    for (std::size_t i = 0; i < text.size(); i += 4) {
        const bool last = i + 4 == text.size();
        const char c0 = text[i];
        const char c1 = text[i + 1];
        const char c2 = text[i + 2];
        const char c3 = text[i + 3];
        const int v0 = DecodeBase64Char(c0);
        const int v1 = DecodeBase64Char(c1);
        if (v0 < 0 || v1 < 0) return false;
        if (c2 == '=') {
            if (!last || c3 != '=') return false;
            out->push_back(static_cast<unsigned char>((v0 << 2) | (v1 >> 4)));
            continue;
        }
        const int v2 = DecodeBase64Char(c2);
        if (v2 < 0) return false;
        out->push_back(static_cast<unsigned char>((v0 << 2) | (v1 >> 4)));
        out->push_back(static_cast<unsigned char>(((v1 & 0x0F) << 4) | (v2 >> 2)));
        if (c3 == '=') {
            if (!last) return false;
            continue;
        }
        const int v3 = DecodeBase64Char(c3);
        if (v3 < 0) return false;
        out->push_back(static_cast<unsigned char>(((v2 & 0x03) << 6) | v3));
    }
    return true;
}

std::string Base64Encode(const std::vector<unsigned char>& data) {
    return Base64Encode(data.data(), data.size());
}

// ---- platform encryption ------------------------------------------------

bool Protect(const std::string& plaintext, std::string* encoded, std::string* errorCode) {
#if defined(_WIN32)
    DATA_BLOB input{};
    input.cbData = static_cast<DWORD>(plaintext.size());
    input.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(plaintext.data()));
    DATA_BLOB output{};
    if (!CryptProtectData(&input, L"SigFlow agent model credentials", nullptr, nullptr,
                          nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output)) {
        if (errorCode != nullptr) *errorCode = "key_store_unavailable";
        return false;
    }
    *encoded = Base64Encode(output.pbData, output.cbData);
    SecureZeroMemory(output.pbData, output.cbData);
    LocalFree(output.pbData);
    return true;
#else
    (void)plaintext;
    (void)encoded;
    if (errorCode != nullptr) *errorCode = "unsupported_platform";
    return false;
#endif
}

bool Unprotect(const std::vector<unsigned char>& blob, std::string* plaintext,
               std::string* errorCode) {
#if defined(_WIN32)
    DATA_BLOB input{};
    input.cbData = static_cast<DWORD>(blob.size());
    input.pbData = const_cast<BYTE*>(blob.data());
    DATA_BLOB output{};
    if (!CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr,
                            CRYPTPROTECT_UI_FORBIDDEN, &output)) {
        if (errorCode != nullptr) *errorCode = "key_store_invalid";
        return false;
    }
    plaintext->assign(reinterpret_cast<const char*>(output.pbData), output.cbData);
    SecureZeroMemory(output.pbData, output.cbData);
    LocalFree(output.pbData);
    return true;
#else
    (void)blob;
    (void)plaintext;
    if (errorCode != nullptr) *errorCode = "unsupported_platform";
    return false;
#endif
}

// ---- atomic file replace ------------------------------------------------

bool AtomicWrite(const std::filesystem::path& path, const std::string& content,
                 std::string* errorCode) {
    std::error_code ec;
    if (path.parent_path().empty()) {
        if (errorCode != nullptr) *errorCode = "key_store_unavailable";
        return false;
    }
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
        if (errorCode != nullptr) *errorCode = "key_store_unavailable";
        return false;
    }
    std::filesystem::path temporary = path;
    temporary += ".tmp";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out) {
            if (errorCode != nullptr) *errorCode = "key_store_write_failed";
            return false;
        }
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
        out.flush();
        if (!out) {
            std::filesystem::remove(temporary, ec);
            if (errorCode != nullptr) *errorCode = "key_store_write_failed";
            return false;
        }
    }
#if defined(_WIN32)
    if (!MoveFileExW(temporary.wstring().c_str(), path.wstring().c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        std::filesystem::remove(temporary, ec);
        if (errorCode != nullptr) *errorCode = "key_store_write_failed";
        return false;
    }
#else
    std::filesystem::rename(temporary, path, ec);
    if (ec) {
        std::filesystem::remove(temporary, ec);
        if (errorCode != nullptr) *errorCode = "key_store_write_failed";
        return false;
    }
#endif
    return true;
}

bool ReadStore(const std::filesystem::path& path, ModelSettings* settings,
               std::string* errorCode) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        *errorCode = "key_store_unavailable";
        return false;
    }
    const std::string content((std::istreambuf_iterator<char>(in)),
                              std::istreambuf_iterator<char>());
    if (content.empty() || content.size() > kMaxStoreBytes) {
        *errorCode = "key_store_invalid";
        return false;
    }
    Json::Value envelope;
    if (!ParseJson(content, &envelope) || !envelope.isObject() ||
        envelope["schema"].asString() != kStoreSchema ||
        !envelope["encrypted"].isString()) {
        *errorCode = "key_store_invalid";
        return false;
    }
    std::vector<unsigned char> blob;
    if (!Base64Decode(envelope["encrypted"].asString(), &blob)) {
        *errorCode = "key_store_invalid";
        return false;
    }
    std::string plaintext;
    if (!Unprotect(blob, &plaintext, errorCode)) return false;
    Json::Value config;
    if (!ParseJson(plaintext, &config) || !config.isObject() ||
        !config["api_key"].isString() || !config["model"].isString() ||
        !config["base_url"].isString()) {
        *errorCode = "key_store_invalid";
        return false;
    }
    settings->apiKey = config["api_key"].asString();
    settings->model = config["model"].asString();
    settings->baseUrl = config["base_url"].asString();
    settings->source = ModelConfigSource::Stored;
    settings->errorCode.clear();
    return true;
}

}  // namespace

const char* ModelConfigSourceId(ModelConfigSource source) {
    switch (source) {
        case ModelConfigSource::Environment: return "environment";
        case ModelConfigSource::Stored: return "stored";
        case ModelConfigSource::EmbeddedDev: return "embedded-dev";
        case ModelConfigSource::None: break;
    }
    return "none";
}

std::filesystem::path AgentModelSettings::StorePath(
    const std::filesystem::path& appDataRoot) {
    return appDataRoot / "sigflow-edu-agent" / "model.credentials.json";
}

bool AgentModelSettings::Supported() {
#if defined(_WIN32)
    return true;
#else
    return false;
#endif
}

bool AgentModelSettings::Validate(const ModelSettings& settings, std::string* errorCode) {
    const auto fail = [errorCode](const char* code) {
        if (errorCode != nullptr) *errorCode = code;
        return false;
    };
    if (!PrintableAscii(settings.apiKey, 512)) return fail("invalid_api_key");
    if (!PrintableAscii(settings.model, 128)) return fail("invalid_model");
    if (settings.baseUrl != "https://api.deepseek.com" &&
        settings.baseUrl != "https://api.deepseek.com/v1") {
        return fail("invalid_base_url");
    }
    if (errorCode != nullptr) errorCode->clear();
    return true;
}

ModelSettings AgentModelSettings::Load(const std::filesystem::path& appDataRoot) {
    ModelSettings settings;

    const bool anyEnvironment =
        EnvPresent("SIGFLOW_EDU_MODEL_PROVIDER") || EnvPresent("SIGFLOW_EDU_MODEL_API_KEY") ||
        EnvPresent("SIGFLOW_EDU_MODEL_BASE_URL") || EnvPresent("SIGFLOW_EDU_MODEL_NAME") ||
        EnvPresent("DEEPSEEK_API_KEY") || EnvPresent("DEEPSEEK_MODEL") ||
        EnvPresent("DEEPSEEK_BASE_URL");
    if (anyEnvironment) {
        settings.source = ModelConfigSource::Environment;
        settings.apiKey = FirstEnvValue({"SIGFLOW_EDU_MODEL_API_KEY", "DEEPSEEK_API_KEY"});
        settings.model = FirstEnvValue({"SIGFLOW_EDU_MODEL_NAME", "DEEPSEEK_MODEL"});
        settings.baseUrl = FirstEnvValue({"SIGFLOW_EDU_MODEL_BASE_URL", "DEEPSEEK_BASE_URL"});
        if (settings.model.empty() && !settings.apiKey.empty()) settings.model = kDefaultModel;
        if (settings.baseUrl.empty() && !settings.apiKey.empty()) settings.baseUrl = kDefaultBaseUrl;
        std::string error;
        if (!Validate(settings, &error)) settings.errorCode = "invalid_environment_config";
        return settings;
    }

#if defined(_WIN32)
    const std::filesystem::path path = StorePath(appDataRoot);
    std::error_code ec;
    if (!appDataRoot.empty() && std::filesystem::exists(path, ec)) {
        settings.source = ModelConfigSource::Stored;
        if (!ReadStore(path, &settings, &settings.errorCode) ||
            !Validate(settings, &settings.errorCode)) {
            settings.apiKey.clear();
            settings.model.clear();
            settings.baseUrl.clear();
        }
        return settings;
    }
#endif

#if defined(SIGFLOW_EMBED_LOCAL_MODEL)
    settings.source = ModelConfigSource::EmbeddedDev;
    settings.apiKey = local_model_config::api_key;
    settings.model = local_model_config::model;
    settings.baseUrl = local_model_config::base_url;
    std::string error;
    if (!Validate(settings, &error)) settings.errorCode = "invalid_embedded_config";
    return settings;
#else
    return settings;
#endif
}

bool AgentModelSettings::Save(const std::filesystem::path& appDataRoot,
                              const ModelSettings& settings, std::string* errorCode) {
    if (!Supported()) {
        if (errorCode != nullptr) *errorCode = "unsupported_platform";
        return false;
    }
    if (appDataRoot.empty()) {
        if (errorCode != nullptr) *errorCode = "key_store_unavailable";
        return false;
    }
    if (!Validate(settings, errorCode)) return false;

    Json::Value plain(Json::objectValue);
    plain["api_key"] = settings.apiKey;
    plain["model"] = settings.model;
    plain["base_url"] = settings.baseUrl;
    std::string encrypted;
    if (!Protect(WriteJson(plain), &encrypted, errorCode)) return false;

    Json::Value envelope(Json::objectValue);
    envelope["schema"] = kStoreSchema;
    envelope["encrypted"] = encrypted;
    if (!AtomicWrite(StorePath(appDataRoot), WriteJson(envelope), errorCode)) return false;
    if (errorCode != nullptr) errorCode->clear();
    return true;
}

bool AgentModelSettings::Clear(const std::filesystem::path& appDataRoot,
                               std::string* errorCode) {
    if (!Supported()) {
        if (errorCode != nullptr) *errorCode = "unsupported_platform";
        return false;
    }
    if (appDataRoot.empty()) {
        if (errorCode != nullptr) *errorCode = "key_store_unavailable";
        return false;
    }
    const std::filesystem::path path = StorePath(appDataRoot);
    std::error_code ec;
    std::filesystem::remove(path, ec);
    if (ec) {
        if (errorCode != nullptr) *errorCode = "key_store_write_failed";
        return false;
    }
    std::filesystem::path temporary = path;
    temporary += ".tmp";
    std::filesystem::remove(temporary, ec);
    if (errorCode != nullptr) errorCode->clear();
    return true;
}

}  // namespace agent
