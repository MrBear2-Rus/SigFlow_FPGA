#pragma once

#include <eda/api/Types.h>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>
#include <regex>

namespace eda { namespace agent {

// Extension IDs contain the namespace and wire major version. Absence means
// base v1 only. Never infer an extension from process health or tool readiness.
inline std::vector<std::string> NegotiateExtensions(
    const Json& capabilities, const std::vector<std::string>& supported) {
    if (!capabilities.is_object() ||
        capabilities.value("protocol", Json()) != Json("edu.api.v1"))
        throw std::invalid_argument("invalid_capabilities");
    if (!capabilities.contains("extensions")) return {};
    const auto& advertised = capabilities["extensions"];
    if (!advertised.is_array()) throw std::invalid_argument("invalid_extensions");
    const std::regex identifier("^[a-z][a-z0-9_-]*(\\.[a-z][a-z0-9_-]*)+\\.v[1-9][0-9]*$");
    std::set<std::string> selected;
    for (const auto& item : advertised) {
        if (!item.is_string() || !std::regex_match(item.get<std::string>(), identifier))
            throw std::invalid_argument("invalid_extensions");
        const auto id = item.get<std::string>();
        for (const auto& known : supported) if (known == id) selected.insert(id);
    }
    return {selected.begin(), selected.end()};
}

}} // namespace eda::agent
