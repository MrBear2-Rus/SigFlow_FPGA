// NextpnrReport.h
#pragma once

#include <wx/string.h>
#include "NextpnrLogParser.h"

// nextpnr 分析报告生成器
// 将解析后的 NextpnrRunRecord 转换为：
//   1. Terminal 摘要（结构化文本，追加在原始日志之后）
//   2. JSON 分析报告（写入 .analysis.json，可累积做趋势对比）
class NextpnrReport
{
public:
    NextpnrReport();

    // 生成 Terminal 摘要文本（追加在 nextpnr 原始输出之后）
    wxString FormatSummary(const NextpnrRunRecord& record);

    // 生成 JSON 分析报告字符串
    wxString GenerateJson(const NextpnrRunRecord& record);

    // 保存 JSON 报告到文件系统
    bool SaveReport(const NextpnrRunRecord& record, const wxString& filePath);

private:
    // 格式化资源利用率段
    wxString FormatResourceSummary(const NextpnrRunRecord& record);

    // 格式化错误详情段（三层：逐条 -> 根因 -> 下一步建议）
    wxString FormatErrorsSummary(const NextpnrRunRecord& record);

    // 格式化时序报告段
    wxString FormatTimingSummary(const NextpnrRunRecord& record);

    // JSON 字符串转义（处理 \ " \n \r \t）
    wxString EscapeJson(const wxString& text);
};
