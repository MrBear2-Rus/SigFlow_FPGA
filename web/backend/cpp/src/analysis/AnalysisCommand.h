#pragma once
#include <eda/api/Types.h>
#include <functional>
namespace eda::analysis {
int AnalysisCommand(int argc, char** argv, const std::function<void(const Json&)>& emit);
}  // namespace eda::analysis
