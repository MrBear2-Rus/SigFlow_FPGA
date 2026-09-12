// NextpnrLogParser.h
#pragma once

#include <wx/string.h>
#include <wx/datetime.h>
#include <vector>
#include <map>

// 单条 nextpnr 日志事件
struct NextpnrLogEvent {
    wxString rawLine;            // 原始行文本
    wxString category;           // info / warning / error / stage / resource / timing
    wxString stage;              // pack / place / route / unknown
    wxString chineseDesc;        // 中文解读
    wxString suggestion;         // 用户操作建议
    wxString extractedDetail;    // 从日志中提取的关键信息（信号名、路径等）
    int lineNumber = 0;          // 行号
};

// nextpnr 运行记录（每次 PnR 运行一条）
struct NextpnrRunRecord {
    wxString toolName;           // "nextpnr-himbaechel"
    wxString toolVersion;        // 从日志首行提取
    wxString deviceName;         // GW1NR-LV9QN88PC6/I5
    wxString familyName;         // GW1N-9C
    wxString executablePath;
    wxString workingDir;
    wxString arguments;

    wxDateTime startTime;
    wxDateTime endTime;
    int exitCode = -1;
    long processId = 0;

    wxString stdoutRaw;          // 完整原始 stdout
    wxString stderrRaw;          // 完整原始 stderr

    // 解析后的阶段状态
    bool parsed = false;
    bool packCompleted = false;    // 打包完成
    bool placeCompleted = false;   // 布局完成
    bool routeCompleted = false;   // 布线完成

    // 时序结果
    double maxFrequencyMHz = 0.0;
    wxString clockName;
    bool timingPassed = false;

    // 资源利用率: 资源名 -> {used, total, percent}
    struct ResourceUsage {
        int used = 0;
        int total = 0;
        double percent = 0.0;
    };
    std::map<wxString, ResourceUsage> resources;

    // 已分类的结构化错误列表（供报告生成器逐条输出）
    std::vector<NextpnrLogEvent> classifiedErrors;

    int warningCount = 0;        // 告警总数
    int errorCount = 0;          // 错误总数
};

// nextpnr 日志解析器
// 将 nextpnr-himbaechel 的 stdout 解析为结构化数据：
//   阶段识别（Pack/Place/Route/Resource/Timing）
//   错误分类（内置 11 条错误字典的预编译正则引擎）
//   资源提取（IOB/LUT4/DFF/BUFG 等）
//   时序提取（最大时钟频率 + 关键路径）
class NextpnrLogParser
{
public:
    NextpnrLogParser();

    // 解析完整 stdout，填充 NextpnrRunRecord
    // stdoutRaw: nextpnr 的原始 stdout（支持 \r\n 和 \n 换行）
    // stderrRaw: nextpnr 的原始 stderr（当前暂未单独处理）
    // 返回 true 表示解析完成（包括空输入等异常情况）
    bool Parse(const wxString& stdoutRaw, const wxString& stderrRaw, NextpnrRunRecord& record);

    // 获取解析出的结构化事件列表（用于报告展示）
    const std::vector<NextpnrLogEvent>& GetEvents() const { return m_events; }

    // 获取错误字典中匹配到的错误列表（不含兜底的"未识别错误"？含）
    const std::vector<NextpnrLogEvent>& GetErrors() const { return m_errors; }

    // 从全文本提取最终的错误/告警计数（Parse 末尾调用一次）
    static void ExtractFinalCounts(const wxString& fullText, NextpnrRunRecord& record);

private:
    // 根据日志行关键词判定当前阶段
    wxString DetectStage(const wxString& line);

    // 解析资源利用率行（格式 "IOB: 5/276 1%"）
    bool ParseResourceLine(const wxString& line, NextpnrRunRecord& record);

    // 用预编译的正则缓存匹配错误类型并提取详细信息
    void ClassifyError(const wxString& line, int lineNumber);

    // 从日志首行提取 nextpnr 版本号
    wxString ExtractVersion(const wxString& firstLine);

    // 从 "Using uarch 'gowin' for device 'xxx'" 行提取器件名
    void ExtractDeviceInfo(const wxString& line, NextpnrRunRecord& record);

    // 提取时序信息（Max frequency）+ Pack/Place/Route 完成标记
    void ExtractTimingInfo(const wxString& line, NextpnrRunRecord& record);

    // 逐行调用时不做操作（有 Parse 末尾的 ExtractFinalCounts 统一处理）
    void ExtractCounts(const wxString& line, NextpnrRunRecord& record);

    std::vector<NextpnrLogEvent> m_events;   // 解析出的所有事件
    std::vector<NextpnrLogEvent> m_errors;   // 匹配到的错误（含兜底的"未识别错误"）
};
