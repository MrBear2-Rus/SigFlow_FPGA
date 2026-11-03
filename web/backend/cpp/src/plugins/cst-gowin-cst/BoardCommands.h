#pragma once
#include <eda/api/Types.h>
#include <functional>
namespace eda::web {
int BoardCommand(int argc, char** argv, const std::function<void(const Json&)>& emit,
                 const std::string& defaultDataDir);
}  // namespace eda::web
