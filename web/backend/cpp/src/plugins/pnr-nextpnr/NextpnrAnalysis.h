#pragma once
#include <eda/api/Types.h>
#include <string>

namespace eda::pnr {
// Metrics are observations from a bounded merged log, not proof of task success.
Json AnalyzeNextpnrLog(const std::string& output, bool outputTruncated, const std::string& device,
                       const std::string& family, int exitCode);
}  // namespace eda::pnr
