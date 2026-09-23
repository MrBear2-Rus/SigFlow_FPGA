#pragma once

#include <string>

#include "Types.h"

namespace eda {

// JobReport / JobRequest / manifest / target-profile 等写盘前统一过此接口校验。
// P0 实现为 JSON Schema 子集（type + required）；完整 schema 校验后续替换实现。
class ISchemaRegistry {
public:
    virtual ~ISchemaRegistry() = default;

    virtual bool RegisterSchema(const std::string& id, const Json& schema) = 0;
    virtual bool HasSchema(const std::string& id) const = 0;
    virtual Json GetSchema(const std::string& id) const = 0;

    // 校验失败时写入 error 并返回 false。
    virtual bool Validate(const std::string& id, const Json& document,
                          std::string& error) const = 0;
};

} // namespace eda
