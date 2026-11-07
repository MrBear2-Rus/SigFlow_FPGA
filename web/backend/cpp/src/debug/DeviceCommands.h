#pragma once

#include "eda/api/Types.h"

#include <functional>

namespace eda::debug {

int DeviceCommand(int argc, char** argv, const std::function<void(const Json&)>& emit);

}  // namespace eda::debug
