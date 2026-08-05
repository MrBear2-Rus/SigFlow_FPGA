// NextpnrLogParser.cpp

#include "NextpnrLogParser.h"
#include <wx/regex.h>
#include <wx/tokenzr.h>
#include <wx/string.h>
#include <vector>

namespace {

constexpr int kMaxLogLines = 50000;    // 日志行数安全上限

// ── 预编译正则缓存（懒初始化，避免重复构造 wxRegEx）──
struct ErrorReCache {
    wxRegEx cstMissing;      // "Failed to open CST file'(.+?)': no such file or directory"
    wxRegEx jsonMissing;     // "Failed to open '--json' file'(.+?)': no such file or directory"
    wxRegEx wrongDevice;     // "No package for partnumber (.+)"
    wxRegEx noFamily;        // "For the GW1N-9 series you need to specify --vopt family="
    wxRegEx unconstrained;   // "Unconstrained IO: (.+)"
    wxRegEx badJson;         // "Failed to parse JSON file'(.+?)'"
    wxRegEx noChipdb;        // "Unable to read chipdb (.+)"
    wxRegEx badVopt;         // "unrecognized --vopt option"
    wxRegEx badCst;          // "Invalid constraint: (.+)"
    wxRegEx routeFail;       // "Failed to route net'(.*?)'"
    wxRegEx success;         // "Program finished normally"

    ErrorReCache() :
        cstMissing(wxT("Failed to open CST file '(.+?)': no such file or directory"),
                   wxRE_ADVANCED | wxRE_ICASE),
        jsonMissing(wxT("Failed to open '--json' file '(.+?)': no such file or directory"),
                    wxRE_ADVANCED | wxRE_ICASE),
        wrongDevice(wxT("No package for partnumber (.+)"), wxRE_ADVANCED | wxRE_ICASE),
        noFamily(wxT("For the GW1N-9 series you need to specify --vopt family="),
                 wxRE_ADVANCED | wxRE_ICASE),
        unconstrained(wxT("Unconstrained IO:\\s*(.+)"), wxRE_ADVANCED | wxRE_ICASE),
        badJson(wxT("Failed to parse JSON file '(.+?)'"), wxRE_ADVANCED | wxRE_ICASE),
        noChipdb(wxT("Unable to read chipdb (.+)"), wxRE_ADVANCED | wxRE_ICASE),
        badVopt(wxT("unrecognized --vopt option"), wxRE_ADVANCED | wxRE_ICASE),
        badCst(wxT("Invalid constraint: (.+)"), wxRE_ADVANCED | wxRE_ICASE),
        routeFail(wxT("Failed to route net '(.*?)'"), wxRE_ADVANCED | wxRE_ICASE),
        success(wxT("Program finished normally"), wxRE_ADVANCED | wxRE_ICASE)
    {}
};

// 懒获取缓存引用（线程安全：C++11 保证 static local 初始化线程安全）
const ErrorReCache& GetCache() { static const ErrorReCache c; return c; }

// 错误字典表：{预编译正则指针, 中文解读, 操作建议}
struct ErrorEntry {
    const wxRegEx* re;
    wxString desc;
    wxString suggestion;
};

const ErrorEntry kErrorTable[] = {
    { &GetCache().cstMissing,  wxT("CST 约束文件不存在"),
      wxT("1.检查 sigflow.project 中 cst= 路径\n2.在 FPGA > Pin Constraints 中生成 CST\n3.确认文件在 constraints/ 目录下") },
    { &GetCache().jsonMissing, wxT("JSON 综合网表不存在"),
      wxT("先运行 FPGA > Synthesis，等待 Yosys 生成 <top>.json") },
    { &GetCache().wrongDevice, wxT("器件型号未被芯片数据库识别"),
      wxT("Tang Nano 9K 标准器件为 GW1NR-LV9QN88PC6/I5，检查 --device 参数") },
    { &GetCache().noFamily,    wxT("缺少芯片系列参数"),
      wxT("在 nextpnr_args 中添加 \"--vopt\", \"family=GW1N-9C\"") },
    { &GetCache().unconstrained, wxT("顶层端口未绑定物理管脚"),
      wxT("在 CST 中为所有顶层端口添加 IO_LOC 绑定") },
    { &GetCache().badJson,     wxT("JSON 综合网表已损坏"),
      wxT("清理 yosys/ 目录后重新运行 Synthesis") },
    { &GetCache().noChipdb,    wxT("Gowin 芯片数据库文件缺失"),
      wxT("确认 external/fpga-tools/runtime/nextpnr/share/himbaechel/gowin/chipdb-GW1N-9C.bin 存在") },
    { &GetCache().badVopt,     wxT("--vopt 参数名写错"),
      wxT("nextpnr-himbaechel 只支持 family 和 cst 两个 vopt 参数") },
    { &GetCache().badCst,      wxT("CST 约束语法无效"),
      wxT("检查 CST 行格式：只接受 IO_LOC / IO_PORT，注释以 # 开头") },
    { &GetCache().routeFail,   wxT("某条线网无法布线"),
      wxT("可能是管脚绑定导致的布线拥塞，尝试调整管脚分配") },
    { &GetCache().success,     wxT("nextpnr 布局布线成功完成"),
      wxT("可继续运行 Apicula 打包生成 .fs 比特流") },
};

constexpr int kErrorTableSize = sizeof(kErrorTable) / sizeof(kErrorTable[0]);

} // anonymous namespace

// 构造函数
NextpnrLogParser::NextpnrLogParser() { }

// 解析 nextpnr stdout，填充结构化记录
bool NextpnrLogParser::Parse(const wxString& stdoutRaw, const wxString& /*stderrRaw*/,
                              NextpnrRunRecord& record)
{
    m_events.clear();
    m_errors.clear();
    record.parsed = false;
    record.packCompleted = false;
    record.placeCompleted = false;
    record.routeCompleted = false;
    record.resources.clear();

    // 空输入保护
    if (stdoutRaw.IsEmpty()) {
        record.parsed = true;
        record.errorCount = 1;
        NextpnrLogEvent emptyErr;
        emptyErr.category = wxT("error");
        emptyErr.chineseDesc = wxT("nextpnr 无任何输出");
        emptyErr.suggestion = wxT("检查 nextpnr 可执行文件是否正常，以及命令行参数是否正确");
        m_errors.push_back(emptyErr);
        record.classifiedErrors = m_errors;
        return false;
    }

    // 统一换行符（处理 Windows \r\n 和 Unix \n）
    wxString normalized = stdoutRaw;
    normalized.Replace(wxT("\r\n"), wxT("\n"));
    normalized.Replace(wxT("\r"), wxT("\n"));

    wxStringTokenizer tokenizer(normalized, wxT("\n"));
    int lineNumber = 0;
    bool firstLine = true;
    bool truncated = false;

    // 逐行解析
    while (tokenizer.HasMoreTokens()) {
        if (lineNumber >= kMaxLogLines) { truncated = true; break; }
        wxString line = tokenizer.GetNextToken();
        line.Trim(true).Trim(false);
        ++lineNumber;
        if (line.IsEmpty()) continue;

        NextpnrLogEvent event;
        event.rawLine = line;
        event.lineNumber = lineNumber;
        event.stage = DetectStage(line);

        // 按行首关键字分类
        if (line.StartsWith(wxT("ERROR:")) || line.StartsWith(wxT("Error:"))) {
            event.category = wxT("error");
            ++record.errorCount;        // 初步计数，末尾会被 ExtractFinalCounts 覆盖
            ClassifyError(line, lineNumber);
        } else if (line.StartsWith(wxT("Warning:")) || line.StartsWith(wxT("WARNING:"))) {
            event.category = wxT("warning");
            ++record.warningCount;
        } else if (event.stage != wxT("unknown")) {
            event.category = wxT("stage");
        } else {
            event.category = wxT("info");
        }

        // 首行提取版本号
        if (firstLine) { record.toolVersion = ExtractVersion(line); firstLine = false; }

        // 逐行提取关键信息
        ExtractDeviceInfo(line, record);
        ExtractTimingInfo(line, record);
        ParseResourceLine(line, record);

        m_events.push_back(event);
    }

    // 全文本精确提取最终计数（覆盖逐行累计的初步计数）
    NextpnrLogParser::ExtractFinalCounts(normalized, record);

    // 截断标记
    if (truncated) {
        ++record.warningCount;
        NextpnrLogEvent truncEvt;
        truncEvt.category = wxT("warning");
        truncEvt.chineseDesc = wxT("日志过长已截断");
        truncEvt.suggestion = wxString::Format(
            wxT("超过 %d 行，仅解析前 %d 行。完整日志见 Terminal 原始输出。"),
            kMaxLogLines, kMaxLogLines);
        m_errors.push_back(truncEvt);
    }

    record.parsed = true;
    record.classifiedErrors = m_errors;
    return true;
}

// 阶段识别：根据日志行关键词判定
wxString NextpnrLogParser::DetectStage(const wxString& line)
{
    // Pack 阶段
    if (line.Contains(wxT("Pack IOBs")) || line.Contains(wxT("Pack GSR")) ||
        line.Contains(wxT("Pack wide LUTs")) || line.Contains(wxT("Pack ALUs")) ||
        line.Contains(wxT("Pack PLL")))
        return wxT("pack");
    if (line.Contains(wxT("Pack")) && line.Contains(wxT("cells")))
        return wxT("pack");

    // Place 阶段
    if (line.Contains(wxT("Creating initial analytic placement")) ||
        line.Contains(wxT("Running main analytical placer")) ||
        line.Contains(wxT("HeAP Placer Time")) ||
        line.Contains(wxT("Running simulated annealing placer")))
        return wxT("place");

    // Route 阶段
    if (line.Contains(wxT("Routing globals")) ||
        line.Contains(wxT("Routing..")) ||
        line.Contains(wxT("Setting up routing queue")) ||
        line.Contains(wxT("Routing complete")) ||
        line.Contains(wxT("Router1 time")))
        return wxT("route");

    // 资源报告
    if (line.Contains(wxT("Device utilisation")))
        return wxT("resource");

    // 时序报告
    if (line.Contains(wxT("Max frequency for clock")) ||
        line.Contains(wxT("Critical path report")) ||
        line.Contains(wxT("Slack histogram")))
        return wxT("timing");

    return wxT("unknown");
}

// 资源行解析（静态预编译正则）
bool NextpnrLogParser::ParseResourceLine(const wxString& line, NextpnrRunRecord& record)
{
    static const wxRegEx re(wxT("^\\s*(\\w+)\\s*:\\s*(\\d+)\\s*/\\s*(\\d+)\\s+(\\d+)%"), wxRE_ADVANCED);
    if (!re.Matches(line)) return false;

    wxString name = re.GetMatch(line, 1);
    long used = 0, total = 0, pct = 0;
    if (!re.GetMatch(line, 2).ToLong(&used)) return false;
    if (!re.GetMatch(line, 3).ToLong(&total)) return false;
    re.GetMatch(line, 4).ToLong(&pct);

    NextpnrRunRecord::ResourceUsage usage;
    usage.used   = static_cast<int>(used);
    usage.total  = static_cast<int>(total);
    usage.percent = static_cast<double>(pct);
    record.resources[name] = usage;
    return true;
}

// 错误分类：遍历预编译的正则缓存，第一匹配即返回
void NextpnrLogParser::ClassifyError(const wxString& line, int lineNumber)
{
    for (int i = 0; i < kErrorTableSize; ++i) {
        const auto& entry = kErrorTable[i];
        if (entry.re->IsValid() && entry.re->Matches(line)) {
            NextpnrLogEvent errEvent;
            errEvent.rawLine     = line;
            errEvent.lineNumber  = lineNumber;
            errEvent.category    = wxT("error");
            errEvent.chineseDesc = entry.desc;
            errEvent.suggestion  = entry.suggestion;

            // 提取第一个捕获组（信号名/路径/器件名等关键信息）
            if (entry.re->GetMatchCount() >= 2) {
                wxString detail = entry.re->GetMatch(line, 1);
                detail.Trim(true).Trim(false);
                if (!detail.IsEmpty() && detail.length() < 200)
                    errEvent.extractedDetail = detail;
            }
            m_errors.push_back(errEvent);
            return;
        }
    }

    // 兜底：未匹配的错误类型
    NextpnrLogEvent unknownErr;
    unknownErr.rawLine     = line;
    unknownErr.lineNumber  = lineNumber;
    unknownErr.category    = wxT("error");
    unknownErr.chineseDesc = wxT("未识别的 nextpnr 错误，请查看原始日志");
    unknownErr.suggestion  = wxT("对照 nextpnr 文档排查，或联系负责人");
    m_errors.push_back(unknownErr);
}

// 版本号提取
wxString NextpnrLogParser::ExtractVersion(const wxString& line)
{
    static const wxRegEx re(wxT("Version\\s+(\\S+)\\)"), wxRE_ADVANCED);
    if (re.Matches(line)) return re.GetMatch(line, 1);
    return wxT("unknown");
}

// 器件信息提取
void NextpnrLogParser::ExtractDeviceInfo(const wxString& line, NextpnrRunRecord& record)
{
    static const wxRegEx re(wxT("Using uarch '([^']+)' for device '([^']+)'"), wxRE_ADVANCED);
    if (re.Matches(line)) record.deviceName = re.GetMatch(line, 2);
}

// 时序 + Pack/Place/Route 完成标记提取
void NextpnrLogParser::ExtractTimingInfo(const wxString& line, NextpnrRunRecord& record)
{
    // 最大时钟频率（post-route 的值会覆盖 placement 阶段的 pre-route 值）
    static const wxRegEx re(wxT("Max frequency for clock '([^']+)':\\s*([\\d.]+)\\s*MHz\\s*\\((PASS|FAIL)"),
                             wxRE_ADVANCED);
    if (re.Matches(line)) {
        record.clockName = re.GetMatch(line, 1);
        wxString f = re.GetMatch(line, 2);
        f.ToDouble(&record.maxFrequencyMHz);
        record.timingPassed = (re.GetMatch(line, 3) == wxT("PASS"));
    }

    // 阶段完成标记
    if (line.Contains(wxT("Pack IOBs")))        record.packCompleted = true;
    if (line.Contains(wxT("HeAP Placer Time")))  record.placeCompleted = true;
    if (line.Contains(wxT("Routing complete")))  record.routeCompleted = true;
}

// 逐行 Counts 不做操作（由 Parse 末尾的 ExtractFinalCounts 统一处理）
void NextpnrLogParser::ExtractCounts(const wxString& /*line*/, NextpnrRunRecord& /*record*/)
{
}

// 全文本提取最终错误/告警计数
void NextpnrLogParser::ExtractFinalCounts(const wxString& fullText, NextpnrRunRecord& record)
{
    static const wxRegEx re(wxT("(\\d+)\\s+warnings?,\\s*(\\d+)\\s+errors?"), wxRE_ADVANCED | wxRE_ICASE);
    if (re.Matches(fullText)) {
        long w = 0, e = 0;
        re.GetMatch(fullText, 1).ToLong(&w);
        re.GetMatch(fullText, 2).ToLong(&e);
        record.warningCount = static_cast<int>(w);
        record.errorCount   = static_cast<int>(e);
    }
}
