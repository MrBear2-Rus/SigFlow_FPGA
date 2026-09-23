#include "SchemaRegistry.h"

namespace eda {
namespace {

const char* JsonTypeName(const Json& value) {
    if (value.is_object()) return "object";
    if (value.is_array()) return "array";
    if (value.is_string()) return "string";
    if (value.is_boolean()) return "boolean";
    if (value.is_number()) return "number";
    if (value.is_null()) return "null";
    return "unknown";
}

} // namespace

bool SimpleSchemaRegistry::RegisterSchema(const std::string& id, const Json& schema) {
    if (id.empty() || !schema.is_object()) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    schemas_[id] = schema;
    return true;
}

bool SimpleSchemaRegistry::HasSchema(const std::string& id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return schemas_.find(id) != schemas_.end();
}

Json SimpleSchemaRegistry::GetSchema(const std::string& id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = schemas_.find(id);
    return it == schemas_.end() ? Json{} : it->second;
}

bool SimpleSchemaRegistry::Validate(const std::string& id, const Json& document,
                                    std::string& error) const {
    Json schema;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = schemas_.find(id);
        if (it == schemas_.end()) {
            error = "schema not registered: " + id;
            return false;
        }
        schema = it->second;
    }

    if (schema.contains("type") && schema["type"].is_string()) {
        const std::string expected = schema["type"].get<std::string>();
        const std::string actual = JsonTypeName(document);
        if (expected != actual) {
            error = "type mismatch: expected '" + expected + "', got '" + actual + "'";
            return false;
        }
    }

    if (schema.contains("required") && schema["required"].is_array()) {
        if (!document.is_object()) {
            error = "'required' present but document is not an object";
            return false;
        }
        for (const auto& key : schema["required"]) {
            if (!key.is_string()) {
                continue;
            }
            const std::string name = key.get<std::string>();
            if (!document.contains(name)) {
                error = "missing required field: " + name;
                return false;
            }
        }
    }

    return true;
}

} // namespace eda
