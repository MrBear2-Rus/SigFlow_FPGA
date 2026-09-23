#pragma once

#include "NextpnrLogModel.h"

#include <string>

namespace eda {
namespace pnr {

// 自 `main/fpga/NextpnrReport` 移入并去 wx 化。
class NextpnrReport {
public:
    NextpnrReport();

    std::string FormatSummary(const NextpnrRunRecord& record);
    std::string GenerateJson(const NextpnrRunRecord& record);
    bool SaveReport(const NextpnrRunRecord& record, const std::string& filePath);

private:
    std::string FormatResourceSummary(const NextpnrRunRecord& record);
    std::string FormatErrorsSummary(const NextpnrRunRecord& record);
    std::string FormatTimingSummary(const NextpnrRunRecord& record);
    static std::string EscapeJson(const std::string& text);
};

} // namespace pnr
} // namespace eda
