#pragma once
#include <eda/api/Types.h>
#include <functional>
namespace eda::wave {
int WaveCommand(int argc, char** argv, const std::function<void(const Json&)>& emit);
}
