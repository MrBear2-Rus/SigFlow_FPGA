#include "SchemaRegistry.h"

#include <utility>
#include <regex>
#include <set>
#include <cmath>

namespace eda {
namespace {

constexpr int kMaxRefDepth = 16;

// 最小 JSON Pointer 解析（RFC 6901 的 '/' 分隔 + ~0/~1 转义），用于 `#/$defs/x` 这类引用。
bool ResolveJsonPointer(const Json& root, const std::string& pointer, Json& out) {
    if (pointer.empty()) {
        out = root;
        return true;
    }
    if (pointer[0] != '/') return false;
    const Json* current = &root;
    std::size_t start = 1;
    while (start <= pointer.size()) {
        const std::size_t end = pointer.find('/', start);
        std::string token = pointer.substr(
            start, end == std::string::npos ? std::string::npos : end - start);
        std::string decoded;
        decoded.reserve(token.size());
        for (std::size_t index = 0; index < token.size(); ++index) {
            if (token[index] == '~' && index + 1 < token.size()) {
                if (token[index + 1] == '0') {
                    decoded.push_back('~');
                    ++index;
                    continue;
                }
                if (token[index + 1] == '1') {
                    decoded.push_back('/');
                    ++index;
                    continue;
                }
            }
            decoded.push_back(token[index]);
        }
        if (current->is_object()) {
            const auto it = current->find(decoded);
            if (it == current->end()) return false;
            current = &(*it);
        } else if (current->is_array()) {
            std::size_t index = 0;
            try {
                index = static_cast<std::size_t>(std::stoull(decoded));
            } catch (const std::exception&) {
                return false;
            }
            if (index >= current->size()) return false;
            current = &(*current)[index];
        } else {
            return false;
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    out = *current;
    return true;
}

const char* JsonTypeName(const Json& value) {
    if (value.is_object()) return "object";
    if (value.is_array()) return "array";
    if (value.is_string()) return "string";
    if (value.is_boolean()) return "boolean";
    // JSON Schema 的 integer 是 number 的子集；解析库不区分，按数值宽度判定。
    if (value.is_number_integer() || value.is_number_unsigned()) return "integer";
    if (value.is_number()) return "number";
    if (value.is_null()) return "null";
    return "unknown";
}

// 校验单个值的 "type"/"enum"，并递归到 properties/items/$ref。
// 语义：properties 中未列出的键视为可选，不报错（契约要求"未知可选字段"可被忽略）；
// "enum" 严格校验（未知枚举值必须显式失败，spec §5.1）。
bool ValidateValue(const Json& schema, const Json& rootSchema, const Json& value,
                   const std::string& path,
                   const SimpleSchemaRegistry::RefResolver& resolver, bool strictRefs, int depth,
                   std::string& error) {
    if (schema.is_boolean()) {
        if (!schema.get<bool>()) error = "false schema at " + path;
        return schema.get<bool>();
    }
    if (!schema.is_object()) { error = "invalid schema at " + path; return false; }
    if (depth >= 64) { error = "schema nesting too deep"; return false; }
    for (const char* keyword : {"allOf", "anyOf", "oneOf"}) {
        if (!schema.contains(keyword)) continue;
        if (!schema[keyword].is_array() || schema[keyword].empty()) {
            error = "invalid combinator at " + path; return false;
        }
        std::size_t matches = 0;
        for (const auto& branch : schema[keyword]) {
            std::string branchError;
            if (ValidateValue(branch, rootSchema, value, path, resolver, strictRefs,
                              depth + 1, branchError)) ++matches;
        }
        if ((std::string(keyword) == "allOf" && matches != schema[keyword].size()) ||
            (std::string(keyword) == "anyOf" && matches == 0) ||
            (std::string(keyword) == "oneOf" && matches != 1)) {
            error = std::string(keyword) + " mismatch at " + path; return false;
        }
    }
    if (schema.contains("not")) {
        std::string ignored;
        if (ValidateValue(schema["not"], rootSchema, value, path, resolver, strictRefs,
                          depth + 1, ignored)) { error = "not mismatch at " + path; return false; }
    }

    // `$ref`：解析后按目标 schema 继续校验（深度上限同时充当循环保护）。
    //   "#/$defs/x"                 -> 当前 schema 文档内部指针
    //   "other.schema.json"         -> 由 resolver 提供的另一份文档
    //   "other.schema.json#/$defs/x"-> 另一份文档内部的指针
    if (schema.contains("$ref") && schema["$ref"].is_string()) {
        const std::string reference = schema["$ref"].get<std::string>();
        const std::string at = path.empty() ? std::string("$") : path;
        if (depth >= kMaxRefDepth) {
            error = "unresolved $ref chain too deep at " + at + ": " + reference;
            return false;
        }
        const std::size_t hash = reference.find('#');
        const std::string filePart = reference.substr(0, hash);
        const std::string fragment = hash == std::string::npos ? std::string()
                                                               : reference.substr(hash + 1);
        Json targetRoot;
        if (filePart.empty()) {
            targetRoot = rootSchema;  // 同文档内部引用
        } else {
            Json document;
            if (!(resolver && resolver(filePart, document) && document.is_object())) {
                if (strictRefs) {
                    error = "unresolved $ref at " + at + ": " + reference;
                    return false;
                }
                return true;  // 非严格模式：无法解析的 $ref 按未知关键字跳过
            }
            targetRoot = std::move(document);
        }
        Json target;
        if (!ResolveJsonPointer(targetRoot, fragment, target) || !target.is_object()) {
            if (strictRefs) {
                error = "unresolvable $ref pointer at " + at + ": " + reference;
                return false;
            }
            return true;
        }
        if (!ValidateValue(target, targetRoot, value, path, resolver, strictRefs, depth + 1,
                           error)) return false;
    }

    if (schema.contains("type")) {
        const Json& typeNode = schema["type"];
        bool typeOk = false;
        const std::string actual = JsonTypeName(value);
        if (typeNode.is_string()) {
            const std::string expected = typeNode.get<std::string>();
            // JSON Schema：integer ⊂ number；非整数值不得冒充 integer。
            typeOk = expected == actual || (expected == "number" && actual == "integer");
        } else if (typeNode.is_array()) {
            for (const auto& candidate : typeNode) {
                if (!candidate.is_string()) continue;
                const std::string expected = candidate.get<std::string>();
                if (expected == actual || (expected == "number" && actual == "integer")) {
                    typeOk = true;
                    break;
                }
            }
        } else {
            typeOk = true;  // 未知 type 声明忽略
        }
        if (!typeOk) {
            error = "type mismatch at " + (path.empty() ? std::string("$") : path) +
                    ": expected '" + typeNode.dump() + "', got '" + actual + "'";
            return false;
        }
    }

    // required 在**每一层**对象上生效（顶层与 $ref 展开后的嵌套对象同等对待）。
    if (schema.contains("required") && schema["required"].is_array() && value.is_object()) {
        for (const auto& key : schema["required"]) {
            if (!key.is_string()) continue;
            const std::string name = key.get<std::string>();
            if (!value.contains(name)) {
                error = "missing required field at " + (path.empty() ? std::string("$") : path) +
                        ": " + name;
                return false;
            }
        }
    }

    if (schema.contains("enum") && schema["enum"].is_array()) {        bool matched = false;
        for (const auto& candidate : schema["enum"]) {
            if (candidate == value) {
                matched = true;
                break;
            }
        }
        if (!matched) {
            error = "value not in enum at " + (path.empty() ? std::string("$") : path) +
                    ": " + value.dump();
            return false;
        }
    }

    if (schema.contains("const")) {
        if (schema["const"] != value) {
            error = "value does not match const at " +
                    (path.empty() ? std::string("$") : path) + ": expected " +
                    schema["const"].dump() + ", got " + value.dump();
            return false;
        }
    }

    if (value.is_number()) {
        for (const char* keyword : {"minimum", "maximum", "exclusiveMinimum", "exclusiveMaximum"}) {
            if (!schema.contains(keyword) || !schema[keyword].is_number()) continue;
            const auto& bound = schema[keyword];
            const std::string key(keyword);
            const bool invalid = key == "minimum" ? value < bound :
                                 key == "maximum" ? value > bound :
                                 key == "exclusiveMinimum" ? value <= bound : value >= bound;
            if (invalid) { error = key + " mismatch at " + path; return false; }
        }
    }
    if (value.is_string()) {
        const std::string text = value.get<std::string>();
        std::size_t length = 0;
        for (unsigned char byte : text) if ((byte & 0xc0) != 0x80) ++length;
        if ((schema.contains("minLength") && length < schema["minLength"].get<std::size_t>()) ||
            (schema.contains("maxLength") && length > schema["maxLength"].get<std::size_t>())) {
            error = "string length mismatch at " + path; return false;
        }
        if (schema.contains("pattern")) {
            try {
                if (!std::regex_search(text, std::regex(schema["pattern"].get<std::string>()))) {
                    error = "pattern mismatch at " + path; return false;
                }
            } catch (const std::regex_error&) { error = "invalid schema pattern"; return false; }
        }
    }
    if (value.is_array()) {
        if ((schema.contains("minItems") && value.size() < schema["minItems"].get<std::size_t>()) ||
            (schema.contains("maxItems") && value.size() > schema["maxItems"].get<std::size_t>())) {
            error = "array length mismatch at " + path; return false;
        }
        if (schema.value("uniqueItems", false)) {
            std::set<std::string> seen;
            for (const auto& item : value) if (!seen.insert(item.dump()).second) {
                error = "duplicate array item at " + path; return false;
            }
        }
    }
    if (value.is_object() && schema.contains("additionalProperties")) {
        const Json properties = schema.value("properties", Json::object());
        for (auto it = value.begin(); it != value.end(); ++it) {
            if (properties.contains(it.key())) continue;
            if (!ValidateValue(schema["additionalProperties"], rootSchema, it.value(),
                               path + "." + it.key(), resolver, strictRefs, depth + 1, error)) return false;
        }
    }

    if (value.is_object() && schema.contains("properties") && schema["properties"].is_object()) {
        const Json& propertySchema = schema["properties"];
        for (auto it = value.begin(); it != value.end(); ++it) {
            const auto found = propertySchema.find(it.key());
            if (found == propertySchema.end()) continue;  // 未声明字段：按契约忽略
            if (!ValidateValue(*found, rootSchema, it.value(), path + "." + it.key(), resolver,
                               strictRefs, depth + 1, error)) {
                return false;
            }
        }
    }

    if (value.is_array() && schema.contains("items")) {
        const Json& itemSchema = schema["items"];
        for (std::size_t index = 0; index < value.size(); ++index) {
            if (!ValidateValue(itemSchema, rootSchema, value[index],
                               path + "[" + std::to_string(index) + "]", resolver, strictRefs,
                               depth + 1, error)) {
                return false;
            }
        }
    }

    return true;
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

void SimpleSchemaRegistry::SetRefResolver(RefResolver resolver) {
    std::lock_guard<std::mutex> lock(mutex_);
    refResolver_ = std::move(resolver);
}

void SimpleSchemaRegistry::SetStrictRefs(bool strict) {
    std::lock_guard<std::mutex> lock(mutex_);
    strictRefs_ = strict;
}

bool SimpleSchemaRegistry::Validate(const std::string& id, const Json& document,
                                    std::string& error) const {
    Json schema;
    RefResolver resolver;
    bool strictRefs = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = schemas_.find(id);
        if (it == schemas_.end()) {
            error = "schema not registered: " + id;
            return false;
        }
        schema = it->second;
        resolver = refResolver_;
        strictRefs = strictRefs_;
    }

    // 值域校验：type/required/enum/const/properties/items（含可解析的 $ref）。
    return ValidateValue(schema, schema, document, std::string(), resolver, strictRefs, 0, error);
}

} // namespace eda
