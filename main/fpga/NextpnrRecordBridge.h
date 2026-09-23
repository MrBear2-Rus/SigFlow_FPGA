#pragma once

#include "NextpnrLogParser.h"   // main（wx）类型

#include "NextpnrLogModel.h"    // 插件（wx-free）类型
#include "NextpnrReportGen.h"

#include "platform/PlatformPaths.h"

// main(wx) ↔ 插件(wx-free) 的 NextpnrRunRecord 转换桥（迁移期使用）。
inline eda::pnr::NextpnrRunRecord ToPluginRecord(const NextpnrRunRecord& src) {
    eda::pnr::NextpnrRunRecord out;
    out.toolName = sigflow::platform::Utf8String(src.toolName);
    out.toolVersion = sigflow::platform::Utf8String(src.toolVersion);
    out.deviceName = sigflow::platform::Utf8String(src.deviceName);
    out.familyName = sigflow::platform::Utf8String(src.familyName);
    out.executablePath = sigflow::platform::Utf8String(src.executablePath);
    out.workingDir = sigflow::platform::Utf8String(src.workingDir);
    out.arguments = sigflow::platform::Utf8String(src.arguments);
    if (src.startTime.IsValid()) {
        out.startTime = sigflow::platform::Utf8String(src.startTime.FormatISOCombined('T'));
    }
    if (src.endTime.IsValid()) {
        out.endTime = sigflow::platform::Utf8String(src.endTime.FormatISOCombined('T'));
    }
    out.exitCode = src.exitCode;
    out.processId = src.processId;
    out.stdoutRaw = sigflow::platform::Utf8String(src.stdoutRaw);
    out.stderrRaw = sigflow::platform::Utf8String(src.stderrRaw);
    out.parsed = src.parsed;
    out.packCompleted = src.packCompleted;
    out.placeCompleted = src.placeCompleted;
    out.routeCompleted = src.routeCompleted;
    out.maxFrequencyMHz = src.maxFrequencyMHz;
    out.clockName = sigflow::platform::Utf8String(src.clockName);
    out.timingPassed = src.timingPassed;
    out.warningCount = src.warningCount;
    out.errorCount = src.errorCount;

    for (const auto& kv : src.resources) {
        eda::pnr::NextpnrRunRecord::ResourceUsage usage;
        usage.used = kv.second.used;
        usage.total = kv.second.total;
        usage.percent = kv.second.percent;
        out.resources[sigflow::platform::Utf8String(kv.first)] = usage;
    }
    for (const auto& event : src.classifiedErrors) {
        eda::pnr::NextpnrLogEvent converted;
        converted.rawLine = sigflow::platform::Utf8String(event.rawLine);
        converted.category = sigflow::platform::Utf8String(event.category);
        converted.stage = sigflow::platform::Utf8String(event.stage);
        converted.chineseDesc = sigflow::platform::Utf8String(event.chineseDesc);
        converted.suggestion = sigflow::platform::Utf8String(event.suggestion);
        converted.extractedDetail = sigflow::platform::Utf8String(event.extractedDetail);
        converted.lineNumber = event.lineNumber;
        out.classifiedErrors.push_back(converted);
    }
    return out;
}

inline void ApplyPluginParseResult(const eda::pnr::NextpnrRunRecord& src, NextpnrRunRecord& dst) {
    dst.toolVersion = wxString::FromUTF8(src.toolVersion.c_str());
    dst.deviceName = wxString::FromUTF8(src.deviceName.c_str());
    dst.parsed = src.parsed;
    dst.packCompleted = src.packCompleted;
    dst.placeCompleted = src.placeCompleted;
    dst.routeCompleted = src.routeCompleted;
    dst.maxFrequencyMHz = src.maxFrequencyMHz;
    dst.clockName = wxString::FromUTF8(src.clockName.c_str());
    dst.timingPassed = src.timingPassed;
    dst.warningCount = src.warningCount;
    dst.errorCount = src.errorCount;
    dst.resources.clear();
    for (const auto& kv : src.resources) {
        NextpnrRunRecord::ResourceUsage usage;
        usage.used = kv.second.used;
        usage.total = kv.second.total;
        usage.percent = kv.second.percent;
        dst.resources[wxString::FromUTF8(kv.first.c_str())] = usage;
    }
    dst.classifiedErrors.clear();
    for (const auto& event : src.classifiedErrors) {
        NextpnrLogEvent converted;
        converted.rawLine = wxString::FromUTF8(event.rawLine.c_str());
        converted.category = wxString::FromUTF8(event.category.c_str());
        converted.stage = wxString::FromUTF8(event.stage.c_str());
        converted.chineseDesc = wxString::FromUTF8(event.chineseDesc.c_str());
        converted.suggestion = wxString::FromUTF8(event.suggestion.c_str());
        converted.extractedDetail = wxString::FromUTF8(event.extractedDetail.c_str());
        converted.lineNumber = event.lineNumber;
        dst.classifiedErrors.push_back(converted);
    }
}
