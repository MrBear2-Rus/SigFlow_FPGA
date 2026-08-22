#pragma once
#include <wx/stc/stc.h>
#include "AsyncAnalysisCenter.h"

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

// --- 语义样式 (Styles) ID 分配 ---
// 0-127 是标准 Lexer 占用或保留区，建议从较后的位置开始自定义
#define STYLE_SEMANTIC_PORT  100
#define STYLE_SEMANTIC_PARAM 101
#define STYLE_SEMANTIC_INST  102
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
    bool OpenFile(wxString path);
    void ClearDocument();
    bool SaveFile();
    bool SaveFileAs(wxString path);
    wxString GetCurrentPath() { return m_currentFilePath; }
};
