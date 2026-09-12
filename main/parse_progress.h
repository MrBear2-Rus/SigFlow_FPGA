// parse_progress.h
// 从工具 stdout 行中检测阶段切换 — 纯函数，零耦合
// 不依赖 UI、不依赖 FPGA 业务逻辑
#pragma once

#include <wx/string.h>
#include <optional>

// 阶段提示——从工具的一行输出中提取的进度信息
struct StageHint {
    wxString stageName;     // 当前阶段名（如 "Synthesis"、"Packing"）
    int stageIndex = 0;     // 阶段序号（0-based）
    int totalStages = 0;    // 预计总阶段数（0=未知）
};

// 从 Yosys 的一行 stdout 中提取阶段信息
// 匹配: "Executing PROC pass"、"Executing SYNTH pass"、"ABC RESULTS" 等
std::optional<StageHint> ParseYosysProgress(const wxString& line);

// 从 nextpnr 的一行 stdout 中提取阶段信息
// 匹配: "Packing"、"Placing"、"Routing"、"Checksum" 等
std::optional<StageHint> ParseNextpnrProgress(const wxString& line);

// 统一入口：根据 toolName 自动分发
std::optional<StageHint> ParseToolProgress(const wxString& toolName, const wxString& line);
