// P1-2：nextpnr 报告生成已移入 InnerPlugin/eda-pnr-nextpnr（wx-free）。
// 本文件保留同签名的 wx 适配层，供 NextpnrExecutor / MainFrame 在迁移期使用。
#include "NextpnrReport.h"

#include "NextpnrRecordBridge.h"
#include "NextpnrReportGen.h"

#include "platform/PlatformPaths.h"

NextpnrReport::NextpnrReport() = default;

wxString NextpnrReport::FormatSummary(const NextpnrRunRecord& record) {
    const eda::pnr::NextpnrRunRecord pluginRecord = ToPluginRecord(record);
    return wxString::FromUTF8(eda::pnr::NextpnrReport().FormatSummary(pluginRecord).c_str());
}

wxString NextpnrReport::GenerateJson(const NextpnrRunRecord& record) {
    const eda::pnr::NextpnrRunRecord pluginRecord = ToPluginRecord(record);
    return wxString::FromUTF8(eda::pnr::NextpnrReport().GenerateJson(pluginRecord).c_str());
}

bool NextpnrReport::SaveReport(const NextpnrRunRecord& record, const wxString& filePath) {
    const eda::pnr::NextpnrRunRecord pluginRecord = ToPluginRecord(record);
    return eda::pnr::NextpnrReport().SaveReport(pluginRecord,
                                                sigflow::platform::Utf8String(filePath));
}
