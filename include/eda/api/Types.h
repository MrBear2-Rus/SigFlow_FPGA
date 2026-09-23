#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace eda {

using Json = nlohmann::json;

enum class ErrorCode {
    None,
    NotFound,
    InvalidArgument,
    TimedOut,
    Cancelled,
    Crashed,
    Internal,
    Unsupported,
};

inline const char* ToString(ErrorCode code) noexcept {
    switch (code) {
        case ErrorCode::None:            return "None";
        case ErrorCode::NotFound:        return "NotFound";
        case ErrorCode::InvalidArgument: return "InvalidArgument";
        case ErrorCode::TimedOut:        return "TimedOut";
        case ErrorCode::Cancelled:       return "Cancelled";
        case ErrorCode::Crashed:         return "Crashed";
        case ErrorCode::Internal:        return "Internal";
        case ErrorCode::Unsupported:     return "Unsupported";
    }
    return "Unknown";
}

struct Error {
    ErrorCode code = ErrorCode::None;
    std::string message;
    std::string hint;

    explicit operator bool() const noexcept { return code != ErrorCode::None; }
    static Error Ok() { return {}; }
};

template <typename... Args>
using Callback = std::function<void(Args...)>;

template <typename T>
using EventHandler = std::function<void(const T&)>;

struct Artifact {
    std::string id;
    std::filesystem::path path;
    std::string schema;
    std::string sha256;
    std::string role;
};

struct MethodCall {
    std::string method;
    Json params;
    std::string requestId;
};

struct PluginInfo {
    std::string id;
    std::string version;
    std::string displayName;
    std::string vendor;
    std::string location;   // "inner" | "external"
    std::string runtime;    // "inprocess" | "process"
    std::vector<std::string> capabilities;
    std::vector<std::string> methods;
    std::vector<std::string> topics;
    std::vector<std::string> platforms;
    std::uint32_t abi = 0;
    std::string buildInfo;
};

inline Json toJson(const Json& value) { return value; }

template <typename T>
T fromJson(const Json& value);

} // namespace eda
