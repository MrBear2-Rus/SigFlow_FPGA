#pragma once
#include <wx/stc/stc.h>
#include "AsyncAnalysisCenter.h"
#include <functional>

enum EDITOR_MODE {Verilog, Markdown, JsonStyle, Text};
#define BLOCK_MARKER_ID 0 // 用于块背景高亮的标记 ID


// Scintilla 支持 0-31，建议从 0 开始
#define INDIC_ERROR      0
#define INDIC_WARNING    1
#define INDIC_HEALTH_AURA 2 // 用于块背景高亮

// --- 状态标记 (Markers) ID 分配 ---
// Scintilla 支持 0-31。注意：0-24 通常由用户定义，25-31 常用于折叠图标
#define MARKER_ID_STABLE     1
#define MARKER_ID_CORRUPTED  2
#define MARKER_ID_INCOMPLETE 3

// --- 语义样式 (Styles) ID 分配 (GAP-A9：唯一来源，禁止在本文件外重复 #define) ---
// 历史：头文件曾定义 100/101/102（从未被引用），.cpp 函数体内又 #define 为 20/21/22（实际生效）。
// 经核实 20/21/22 与 LexVerilog 自带样式冲突：
//   wxSTC_V_COMMENT_WORD = 20、wxSTC_V_INPUT = 21、wxSTC_V_OUTPUT = 22
//   (3rd/wxWidgets-3.2.9/include/wx/stc/stc.h:1487-1489)
// 其中 20 由 LexVerilog 实际发射（注释内关键词，LexVerilog.cxx:567/576），会被静默改色。
// 故统一为独立号段（Scintilla 用户自定义区，wxSTC_STYLE_MAX = 255），保留原 100/101/102 取值。
enum SemanticStyleId {
    STYLE_SEMANTIC_PORT  = 100,
    STYLE_SEMANTIC_PARAM = 101,
    STYLE_SEMANTIC_INST  = 102,
};
#define STYLE_DEFAULT wxSTC_STYLE_DEFAULT

class SigTextEditor : public wxStyledTextCtrl {
public:

    SigTextEditor(wxWindow* parent);
    wxString m_currentFilePath;
    int temp_version;
    EDITOR_MODE mode;
    bool m_isLoading ;
    //LintRes m_latestAnalysis;

    void SetMode(EDITOR_MODE mode);
    void SetHeaderStateCallback(std::function<void(const wxString&, const wxString&, bool)> callback);
    void SetVerilogStyle();
    void SetVerilogIDE();
    void SetMarkdownStyle();
    void SetJsonStyle();
    void SetTextStyle();


    bool VisualFeedBack(const LintResult res);

    //void RenderDiagnosticIndicators(const std::vector<LintMessage>& msgs);
    void RenderLineMarker(const std::vector<Stability> line_status);
    void MarkerDeleteAllByID(int markerId);
    void RenderFoldingStructure(const std::vector<bool> is_lines_header, const std::vector<int> line_depth);


    //void RenderSemanticColors(const std::vector<VerilogBlock>& blocks);

    //void ClearAllFeedBack();

    //void OnMouseDwell(wxStyledTextEvent& event);
    //void ShowReactiveDetail(int pos, const LintMessage& msg);
    void OnMarginClick(wxStyledTextEvent& event);


    void DebugFoldLevels();

    bool SaveIfModified();
    int GetSnapVersion() { return temp_version; };
    void OnTextChanged(wxStyledTextEvent& event);
    void OnDocumentChanged(wxStyledTextEvent& event);
    bool OpenFile(wxString path);
    void ClearDocument();
    bool SaveFile();
    bool SaveFileAs(wxString path);
    wxString GetCurrentPath() { return m_currentFilePath; }

private:
    void NotifyHeaderState(bool validated);
    std::function<void(const wxString&, const wxString&, bool)> m_headerStateCallback;
    wxDECLARE_EVENT_TABLE();
};
