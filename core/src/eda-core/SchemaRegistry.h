#pragma once

#include <functional>
#include <map>
#include <mutex>
#include <string>

#include <eda/api/schemas.hpp>

namespace eda {

// JSON Schema 子集实现：支持 "type"（object/array/string/number/boolean/null）、
// enum/const/required/properties/items/$ref、oneOf/anyOf/allOf/not、pattern、
// 字符/数组长度、数值边界、uniqueItems/additionalProperties。不是完整 Draft 2020-12
// 实现；安全请求仍须作领域校验。递归和引用均有深度上限。
//
// `$ref` 解析由调用方注入（RefResolver）：核心不假设 schema 一定来自文件系统。
// 默认**不**开启严格模式——无法解析的 `$ref` 按"未知关键字"跳过，保持既有行为的
// 向后兼容；需要契约严格性的测试应显式 SetStrictRefs(true) 并注入 resolver。
class SimpleSchemaRegistry final : public ISchemaRegistry {
public:
    // reference 为原始 `$ref` 文本（例如 "design-node.schema.json" 或
    // "schemas/source-ref.schema.json" 或 "#/$defs/foo"）。
    // 成功时 out 为该引用指向的 schema 对象。
    using RefResolver = std::function<bool(const std::string& reference, Json& out)>;

    bool RegisterSchema(const std::string& id, const Json& schema) override;
    bool HasSchema(const std::string& id) const override;
    Json GetSchema(const std::string& id) const override;
    bool Validate(const std::string& id, const Json& document, std::string& error) const override;

    void SetRefResolver(RefResolver resolver);
    // 严格模式：无法解析的 `$ref` 视为校验失败（而不是跳过）。
    void SetStrictRefs(bool strict);

private:
    mutable std::mutex mutex_;
    std::map<std::string, Json> schemas_;
    RefResolver refResolver_;
    bool strictRefs_ = false;
};

} // namespace eda
