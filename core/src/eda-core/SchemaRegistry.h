#pragma once

#include <map>
#include <mutex>
#include <string>

#include <eda/api/schemas.hpp>

namespace eda {

// JSON Schema 子集实现：支持 "type"（object/array/string/number/boolean/null）
// 与 "required"（对象必填字段）。其余关键字忽略；完整校验后续替换实现。
class SimpleSchemaRegistry final : public ISchemaRegistry {
public:
    bool RegisterSchema(const std::string& id, const Json& schema) override;
    bool HasSchema(const std::string& id) const override;
    Json GetSchema(const std::string& id) const override;
    bool Validate(const std::string& id, const Json& document, std::string& error) const override;

private:
    mutable std::mutex mutex_;
    std::map<std::string, Json> schemas_;
};

} // namespace eda
