// P1-2：nextpnr 日志解析已移入 InnerPlugin/eda-pnr-nextpnr（wx-free）。
// 本文件保留同签名的 wx 适配层，供 NextpnrExecutor / MainFrame 在迁移期使用。
#include "NextpnrLogParser.h"

#include "NextpnrLogModel.h"
#include "NextpnrRecordBridge.h"

#include "platform/PlatformPaths.h"

NextpnrLogParser::NextpnrLogParser() = default;

bool NextpnrLogParser::Parse(const wxString& stdoutRaw, const wxString& stderrRaw,
                             NextpnrRunRecord& record) {
    eda::pnr::NextpnrRunRecord pluginRecord = ToPluginRecord(record);
    pluginRecord.stdoutRaw = sigflow::platform::Utf8String(stdoutRaw);
    pluginRecord.stderrRaw = sigflow::platform::Utf8String(stderrRaw);

    eda::pnr::NextpnrLogParser parser;
    const bool ok = parser.Parse(pluginRecord.stdoutRaw, pluginRecord.stderrRaw, pluginRecord);
    ApplyPluginParseResult(pluginRecord, record);
    return ok;
}

void NextpnrLogParser::ExtractFinalCounts(const wxString& fullText, NextpnrRunRecord& record) {
    eda::pnr::NextpnrRunRecord pluginRecord;
    eda::pnr::NextpnrLogParser::ExtractFinalCounts(
        sigflow::platform::Utf8String(fullText), pluginRecord);
    record.warningCount = pluginRecord.warningCount;
    record.errorCount = pluginRecord.errorCount;
}
