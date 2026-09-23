#pragma once

#include <eda/api/schemas.hpp>

namespace eda {

// 把 P0 的核心 schema（eda.job.v1 / eda.jobreport.v1）注册进给定 registry。
void RegisterCoreSchemas(ISchemaRegistry& registry);

} // namespace eda
