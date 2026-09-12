// parse_progress.cpp
// 阶段检测纯函数实现

#include "parse_progress.h"
#include <wx/regex.h>

// ── 预编译正则缓存（同项目 NextpnrLogParser 风格）──
struct ProgressReCache {
    wxRegEx yosysProcPass;
    wxRegEx yosysSynthPass;
    wxRegEx yosysAbcResults;
    wxRegEx yosysTechmapPass;
    wxRegEx yosysWriteOutput;
    wxRegEx yosysReadVerilog;
    wxRegEx yosysHierarchy;
    wxRegEx yosysPrintStats;

    wxRegEx nextpnrPacking;
    wxRegEx nextpnrPlacing;
    wxRegEx nextpnrRouting;
    wxRegEx nextpnrChecksum;
    wxRegEx nextpnrComplete;

    ProgressReCache()
        // ── Yosys（标准输出为英文小写+标题大写）──
        : yosysProcPass(wxT("Executing PROC pass"))
        , yosysSynthPass(wxT("Executing SYNTH pass"))
        , yosysAbcResults(wxT("ABC RESULTS"))
        , yosysTechmapPass(wxT("Executing TECHMAP"))
        , yosysWriteOutput(wxT("Writing.*output|write_json|write_xaiger"))
        , yosysReadVerilog(wxT("read_verilog|Reading.*verilog"))
        , yosysHierarchy(wxT("Executing HIERARCHY|hierarchy"))
        , yosysPrintStats(wxT("Printing statistics|print_stats"))

        // ── nextpnr（输出大小写混合，用 wxRE_ICASE + 单词边界避免误匹配）──
        , nextpnrPacking(wxT("\\bpack(ing)?\\b"), wxRE_ICASE)
        , nextpnrPlacing(wxT("\\bplac(e|ing)\\b|Creating.*placement|analytic placement"), wxRE_ICASE)
        , nextpnrRouting(wxT("\\brout(e|ing)\\b"), wxRE_ICASE)
        , nextpnrChecksum(wxT("\\bchecksum\\b"), wxRE_ICASE)
        , nextpnrComplete(wxT("\\bcomplete\\b|\\bfinished\\b"), wxRE_ICASE)
    {}
};

static ProgressReCache& GetCache()
{
    static ProgressReCache cache;
    return cache;
}

// ── Yosys 阶段检测 ──
// Yosys 按顺序执行 pass，共 6 个可见阶段 → 映射到 0-5
std::optional<StageHint> ParseYosysProgress(const wxString& line)
{
    auto& re = GetCache();
    StageHint hint;
    hint.totalStages = 6;

    if (re.yosysReadVerilog.Matches(line)) {
        hint.stageName = wxT("Reading Verilog");
        hint.stageIndex = 0;
        return hint;
    }
    if (re.yosysHierarchy.Matches(line)) {
        hint.stageName = wxT("Building Hierarchy");
        hint.stageIndex = 1;
        return hint;
    }
    if (re.yosysProcPass.Matches(line)) {
        hint.stageName = wxT("Processes");
        hint.stageIndex = 2;
        return hint;
    }
    if (re.yosysSynthPass.Matches(line)) {
        hint.stageName = wxT("Synthesis");
        hint.stageIndex = 3;
        return hint;
    }
    if (re.yosysAbcResults.Matches(line)) {
        hint.stageName = wxT("ABC Optimization");
        hint.stageIndex = 4;
        return hint;
    }
    if (re.yosysTechmapPass.Matches(line)) {
        hint.stageName = wxT("Technology Mapping");
        hint.stageIndex = 5;
        return hint;
    }
    // 以下两个阶段 index=6 超出 totalStages，AdvanceStage 会 clamp 到 100%
    if (re.yosysWriteOutput.Matches(line)) {
        hint.stageName = wxT("Writing Output");
        hint.stageIndex = 6;
        return hint;
    }
    if (re.yosysPrintStats.Matches(line)) {
        hint.stageName = wxT("Final Statistics");
        hint.stageIndex = 6;
        return hint;
    }

    return std::nullopt;
}

// ── nextpnr 阶段检测 ──
// nextpnr-himbaechel 按 Pack → Place → Route → Checksum → Complete 顺序执行
// 从后往前匹配，避免 "Routing complete" 被 Complete 先行捕获
std::optional<StageHint> ParseNextpnrProgress(const wxString& line)
{
    auto& re = GetCache();
    StageHint hint;
    hint.totalStages = 4;

    // 倒序匹配：更具体的阶段先检查，避免包含关系误判
    if (re.nextpnrComplete.Matches(line)) {
        hint.stageName = wxT("Complete");
        hint.stageIndex = 4;
        return hint;
    }
    if (re.nextpnrChecksum.Matches(line)) {
        hint.stageName = wxT("Checksum");
        hint.stageIndex = 3;
        return hint;
    }
    if (re.nextpnrRouting.Matches(line)) {
        hint.stageName = wxT("Routing");
        hint.stageIndex = 2;
        return hint;
    }
    if (re.nextpnrPlacing.Matches(line)) {
        hint.stageName = wxT("Placement");
        hint.stageIndex = 1;
        return hint;
    }
    if (re.nextpnrPacking.Matches(line)) {
        hint.stageName = wxT("Packing");
        hint.stageIndex = 0;
        return hint;
    }

    return std::nullopt;
}

// ── 统一入口 ──
std::optional<StageHint> ParseToolProgress(const wxString& toolName, const wxString& line)
{
    if (toolName.Lower().Contains(wxT("yosys"))) {
        return ParseYosysProgress(line);
    }
    if (toolName.Lower().Contains(wxT("nextpnr"))) {
        return ParseNextpnrProgress(line);
    }
    // openFPGALoader / Verilator 等无阶段输出，返回 nullopt
    return std::nullopt;
}
