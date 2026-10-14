#include "SigTextEditor.h"
#include "ui/UiTheme.h"
#include <wx/filename.h>

wxBEGIN_EVENT_TABLE(SigTextEditor, wxStyledTextCtrl)
EVT_STC_MODIFIED(wxID_ANY, SigTextEditor::OnTextChanged)
wxEND_EVENT_TABLE()

SigTextEditor::SigTextEditor(wxWindow* parent)
    : wxStyledTextCtrl(parent, wxID_ANY), temp_version(0), mode(EDITOR_MODE::Verilog), m_isLoading(false) {
    SetMode(EDITOR_MODE::Verilog);
    SetCodePage(wxSTC_CP_UTF8);
    SetProperty("fold.compact", "0"); // 不要压缩空行折叠
    SetProperty("fold.comment", "1"); // 如果有注释折叠可以开启
    Bind(wxEVT_STC_MARGINCLICK, &SigTextEditor::OnMarginClick, this);
    // CMP-051: wxEVT_STC_CHANGE is the document-level notification.  It stays
    // independent from consumers listening to low-level MODIFIED events.
    Bind(wxEVT_STC_CHANGE, &SigTextEditor::OnDocumentChanged, this);
}


void SigTextEditor::SetMode(EDITOR_MODE mode) {
    this->mode = mode;
    switch (mode) {
    case EDITOR_MODE::Verilog:
        SetVerilogStyle();
        SetVerilogIDE();
        break;

    case EDITOR_MODE::Markdown:
        SetMarkdownStyle();
        break;
    case EDITOR_MODE::Text:
        SetTextStyle();
        break;
    case EDITOR_MODE::JsonStyle:
        SetJsonStyle();
        break;
}

    // CMP-052: 13 DIP code needs at least 1.5× readable leading on every lexer.
    SetExtraAscent(FromDIP(3));
    SetExtraDescent(FromDIP(3));
}

void SigTextEditor::SetHeaderStateCallback(std::function<void(const wxString&, const wxString&, bool)> callback) {
    m_headerStateCallback = std::move(callback);
    NotifyHeaderState(false);
}

void SigTextEditor::NotifyHeaderState(bool validated) {
    if (!m_headerStateCallback) return;
    wxString language = "Text";
    switch (mode) {
    case EDITOR_MODE::Verilog: language = "Verilog"; break;
    case EDITOR_MODE::Markdown: language = "Markdown"; break;
    case EDITOR_MODE::JsonStyle: language = "JSON"; break;
    case EDITOR_MODE::Text: break;
    }
    m_headerStateCallback(m_currentFilePath, language, validated);
}

void SigTextEditor::SetVerilogStyle() {
    SetLexer(wxSTC_LEX_VERILOG);

    // 取色唯一来源：UiTheme（SAFE-012）。编辑器自身位于 canvas 层级。
    const UiTheme::Colours& pal = UiTheme::Palette();

    // 1. 基础全局样式 (canvas 背景，text1 正文；CMP-052: 13 DIP 等宽)
    wxFont codeFont = UiTheme::MonoFont(this, UiTheme::TextSize::P13);
    StyleSetFont(wxSTC_STYLE_DEFAULT, codeFont);
    StyleSetBackground(wxSTC_STYLE_DEFAULT, pal.canvas); // 编辑器底
    StyleSetForeground(wxSTC_STYLE_DEFAULT, pal.text1);  // 正文墨
    StyleClearAll();

    SetKeyWords(0, "module endmodule always assign begin end if else case for while");
    SetKeyWords(1, "wire reg logic integer parameter localparam genvar");
    SetKeyWords(2, "and nand or nor xor xnor buf not");
    SetKeyWords(3, "$display $monitor $finish $time");
    SetKeyWords(4, "input output inout");
    SetKeyWords(5, "`define `ifdef `ifndef `endif `include");


    // ===============================
    // 基础默认
    // ===============================
    StyleSetForeground(wxSTC_V_DEFAULT, pal.text1);
    StyleSetBackground(wxSTC_V_DEFAULT, pal.canvas);
    StyleClearAll();

    // StyleClearAll() 会复写 margin 样式，故 gutter 必须在其后设置。
    SetMarginType(0, wxSTC_MARGIN_NUMBER);
    SetMarginWidth(0, 45);
    StyleSetBackground(wxSTC_STYLE_LINENUMBER, pal.sunkenOnCanvas);
    StyleSetForeground(wxSTC_STYLE_LINENUMBER, pal.text3);
    SetMarginMask(0, 0);

    // ===============================
    // 结构关键字 (module / always / begin / end)  → kw = accent (CMP-053)
    // ===============================
    StyleSetForeground(wxSTC_V_WORD, pal.accent);
    StyleSetBold(wxSTC_V_WORD, true);

    // ===============================
    // Gate primitives (and/nand/or/...)  → 关键字族 = accent
    // ===============================
    StyleSetForeground(wxSTC_V_WORD3, pal.accent);
    StyleSetBold(wxSTC_V_WORD3, false);

    // ===============================
    // 系统任务 / 系统函数 ($display...) 与类型共用 SCE_V_WORD2
    // ===============================
    StyleSetForeground(wxSTC_V_WORD2, pal.codeType); // ty = codeType（SPEC §3.6 派生值，替代 brand 以满足 4.5:1）
    StyleSetBold(wxSTC_V_WORD2, false);

    // ===============================
    // 端口方向关键字  → kw = accent
    // ===============================
    StyleSetForeground(wxSTC_V_INPUT, pal.accent);
    StyleSetBold(wxSTC_V_INPUT, true);

    StyleSetForeground(wxSTC_V_OUTPUT, pal.accent);
    StyleSetBold(wxSTC_V_OUTPUT, true);

    StyleSetForeground(wxSTC_V_INOUT, pal.accent);
    StyleSetBold(wxSTC_V_INOUT, true);

    // ===============================
    // 类型 / 数据声明 (wire/reg/integer...)  → ty = codeType（SPEC §3.6）
    // ===============================
    StyleSetForeground(wxSTC_V_WORD2, pal.codeType); // 已用于系统任务（Scintilla复用）

    // ===============================
    // 标识符  → text2（沿用原「次级中性灰」的层次意图；无 CMP-053 强制项）
    // ===============================
    StyleSetForeground(wxSTC_V_IDENTIFIER, pal.text2); // 中性深灰
    StyleSetBold(wxSTC_V_IDENTIFIER, false);

    // ===============================
    // 数字 / 常量  → 无专用 token，取色相最近的 danger（近似映射）
    // ===============================
    StyleSetForeground(wxSTC_V_NUMBER, pal.danger); // 红棕

    // ===============================
    // 字符串  → st = warn (CMP-053)
    // ===============================
    StyleSetForeground(wxSTC_V_STRING, pal.warn);  // 棕橙

    StyleSetForeground(wxSTC_V_STRINGEOL, pal.warn);
    StyleSetBackground(wxSTC_V_STRINGEOL, pal.hoverOnPanel); // 近似：base 族无 danger 淡底

    // ===============================
    // 操作符 / 符号
    // ===============================
    StyleSetForeground(wxSTC_V_OPERATOR, pal.text1);

    // ===============================
    // 端口连接名 (.port)
    // ===============================
    StyleSetForeground(wxSTC_V_PORT_CONNECT, pal.accent);
    StyleSetBold(wxSTC_V_PORT_CONNECT, true);

    // ===============================
    // 预处理 / 宏  → 色相最近的 danger（近似映射）
    // ===============================
    StyleSetForeground(wxSTC_V_PREPROCESSOR, pal.danger);
    StyleSetBold(wxSTC_V_PREPROCESSOR, true);

    // ===============================
    // 注释  → cm = 斜体 + codeComment（SPEC §3.6 派生值，替代 text3 以满足 4.5:1）
    // ===============================
    StyleSetForeground(wxSTC_V_COMMENT, pal.codeComment);
    StyleSetForeground(wxSTC_V_COMMENTLINE, pal.codeComment);
    StyleSetForeground(wxSTC_V_COMMENTLINEBANG, pal.codeComment);
    StyleSetItalic(wxSTC_V_COMMENT, true);
    StyleSetItalic(wxSTC_V_COMMENTLINE, true);

    // ===============================
    // 注释中的关键词 (TODO / FIXME)  → 注释族 = codeComment（SPEC §3.6）
    // ===============================
    StyleSetForeground(wxSTC_V_COMMENT_WORD, pal.codeComment);
    StyleSetBold(wxSTC_V_COMMENT_WORD, true);

    // ===============================
    // 用户自定义 (pragma / annotation / IDE hint)  → 色相最近的 warn（近似映射）
    // ===============================
    StyleSetForeground(wxSTC_V_USER, pal.warn); // 橙色
    StyleSetBold(wxSTC_V_USER, true);



    // 5. 编辑器 UI 细节
    SetCaretForeground(pal.text1);            // 插入符 = 正文墨
    SetSelBackground(true, pal.selOnPanel); // 选中底

    // 当前行高亮（极淡中性底）
    SetCaretLineVisible(true);
    SetCaretLineBackground(pal.hoverOnPanel);

    StyleSetForeground(wxSTC_STYLE_BRACELIGHT, pal.accent); // 配对高亮 = 焦点色
    StyleSetBold(wxSTC_STYLE_BRACELIGHT, true);
}

void SigTextEditor::SetVerilogIDE() {
    const UiTheme::Colours& pal = UiTheme::Palette();

    // --- A. Margin 1: 逻辑块健康状态条 ---
    SetMarginWidth(1, 10);
    SetMarginType(1, wxSTC_MARGIN_SYMBOL);
    SetMarginSensitive(1, false);
    SetMarginMask(1, 0xFFFFFFFF);
    SetMarginMask(1, ~wxSTC_MASK_FOLDERS);

    // Marker 1: 稳定态 (品牌绿)
    MarkerDefine(1, wxSTC_MARK_FULLRECT);
    MarkerSetForeground(1, pal.brand);
    MarkerSetBackground(1, pal.brand);

    // Marker 2: 错误态 (危险红)
    MarkerDefine(2, wxSTC_MARK_FULLRECT);
    MarkerSetForeground(2, pal.danger);
    MarkerSetBackground(2, pal.danger);

    // Marker 3: 未完成 (警告橙)
    MarkerDefine(3, wxSTC_MARK_FULLRECT);
    MarkerSetForeground(3, pal.warn);
    MarkerSetBackground(3, pal.warn);

    // --- B. Margin 2: 代码折叠槽 ---
    SetMarginType(2, wxSTC_MARGIN_SYMBOL);
    SetMarginMask(2, wxSTC_MASK_FOLDERS);
    SetMarginWidth(2, 16);
    SetMarginSensitive(2, true);
    SetProperty("fold", "1"); // 激活 Scintilla 折叠引擎

    // 定义经典折叠图标 (加减号)
    //MarkerDefine(wxSTC_MARKNUM_FOLDEROPEN, wxSTC_MARK_BOXMINUS, pal.canvas, pal.text3);
    //MarkerDefine(wxSTC_MARKNUM_FOLDER, wxSTC_MARK_BOXPLUS, pal.canvas, pal.text3);
    //MarkerDefine(wxSTC_MARKNUM_FOLDERSUB, wxSTC_MARK_VLINE, pal.canvas, pal.text3);
    //MarkerDefine(wxSTC_MARKNUM_FOLDERTAIL, wxSTC_MARK_LCORNER, pal.canvas, pal.text3);
    //MarkerDefine(wxSTC_MARKNUM_FOLDEREND, wxSTC_MARK_LCORNER, pal.canvas, pal.text3);
    SetFoldFlags(wxSTC_FOLDFLAG_LINEBEFORE_CONTRACTED | wxSTC_FOLDFLAG_LINEAFTER_CONTRACTED);

    
    
    
    // 【1】自定义：未折叠状态的头（方框减号）
    MarkerDefine(wxSTC_MARKNUM_FOLDEROPEN, wxSTC_MARK_BOXMINUS, pal.canvas, pal.text3);

    // 【2】自定义：已折叠状态的头（方框加号）
    MarkerDefine(wxSTC_MARKNUM_FOLDER, wxSTC_MARK_BOXPLUS, pal.canvas, pal.text3);

    // 【3】自定义：折叠块中间的垂直连线
    MarkerDefine(wxSTC_MARKNUM_FOLDERSUB, wxSTC_MARK_VLINE, pal.canvas, pal.text3);

    // 【4】自定义：展开状态下，块结尾的那个 L 型拐角
    MarkerDefine(wxSTC_MARKNUM_FOLDERTAIL, wxSTC_MARK_LCORNER, pal.canvas, pal.text3);

    // 【5】自定义：折叠状态下，如果下面还有嵌套，结尾显示的那个带连线的方框（极其重要！）
    // 这就是解决你“二级 Header 变圆圈”的关键
    MarkerDefine(wxSTC_MARKNUM_FOLDEREND, wxSTC_MARK_BOXPLUSCONNECTED, pal.canvas, pal.text3);

    // 【6】自定义：处于打开状态的子级头（带连线的减号方框）
    MarkerDefine(wxSTC_MARKNUM_FOLDEROPENMID, wxSTC_MARK_BOXMINUSCONNECTED, pal.canvas, pal.text3);

    // 【7】自定义：由于某些跳变产生的中间连线
    MarkerDefine(wxSTC_MARKNUM_FOLDERMIDTAIL, wxSTC_MARK_VLINE, pal.canvas, pal.text3);
    
    
    
    
    
    
    
    
    
    
    
    
    // --- C. 诊断波浪线配置 (Indicators) ---
    IndicatorSetStyle(0, wxSTC_INDIC_SQUIGGLE); // Error
    IndicatorSetForeground(0, pal.danger);
    IndicatorSetStyle(1, wxSTC_INDIC_SQUIGGLE); // Warning
    IndicatorSetForeground(1, pal.warn);

    // --- D. 语义高亮样式定义 (为后续渲染准备) ---
    // GAP-A9：唯一定义见 SigTextEditor.h 的 SemanticStyleId（100/101/102）。
    StyleSetForeground(STYLE_SEMANTIC_PORT, pal.accent);  // 端口：与端口连接名同族
    StyleSetBold(STYLE_SEMANTIC_PORT, true);
    StyleSetForeground(STYLE_SEMANTIC_PARAM, pal.codeType); // 参数：类型/数据声明族（SPEC §3.6）
    StyleSetForeground(STYLE_SEMANTIC_INST, pal.text2);  // 实例名：标识符族

    // --- E. 交互反馈 ---
    SetMouseDwellTime(500); // 开启 500ms 悬停检测
    SetCaretForeground(pal.text1);
    SetCaretLineVisible(true);
    SetCaretLineBackground(pal.hoverOnPanel);
    SetSelBackground(true, pal.selOnPanel);
}
void SigTextEditor::SetTextStyle() {
    ClearAll();
    SetLexer(wxSTC_LEX_NULL);

    const UiTheme::Colours& pal = UiTheme::Palette();

    // 默认字体：text1 正文、canvas 背景；CMP-052: 13 DIP 等宽
    wxFont font = UiTheme::MonoFont(this, UiTheme::TextSize::P13);
    StyleSetFont(wxSTC_STYLE_DEFAULT, font);
    StyleSetBackground(wxSTC_STYLE_DEFAULT, pal.canvas); // 编辑器底
    StyleSetForeground(wxSTC_STYLE_DEFAULT, pal.text1);  // 正文墨

    StyleClearAll();

    // 配置行号列 (Margin 0)：凹陷底 + 辅助灰数字
    SetMarginType(0, wxSTC_MARGIN_NUMBER);
    SetMarginWidth(0, 45);
    StyleSetForeground(wxSTC_STYLE_LINENUMBER, pal.text3);
    StyleSetBackground(wxSTC_STYLE_LINENUMBER, pal.sunkenOnCanvas);

    // 设置插入符为正文墨
    SetCaretForeground(pal.text1);
}

void SigTextEditor::SetMarkdownStyle() {
    SetLexer(wxSTC_LEX_MARKDOWN);

    const UiTheme::Colours& pal = UiTheme::Palette();

    // 基础继承
    StyleSetBackground(wxSTC_STYLE_DEFAULT, pal.canvas);
    StyleSetForeground(wxSTC_STYLE_DEFAULT, pal.text1);
    StyleClearAll();

    // 标题族（样式号 1/2/3）→ accent
    StyleSetForeground(1, pal.accent);  // H1
    StyleSetBold(1, true);
    StyleSetForeground(2, pal.accent);  // H2
    StyleSetForeground(3, pal.accent);  // H3

    // 代码块（样式号 19/20）→ 色相最近的 danger（近似映射）
    StyleSetForeground(19, pal.danger);
    StyleSetForeground(20, pal.danger);

    // 列表（样式号 15）→ 绿族 codeType（近似映射；SPEC §3.6）
    StyleSetForeground(15, pal.codeType);

    // 粗体/斜体
    StyleSetBold(10, true);
    StyleSetItalic(11, true);

    // 链接族（样式号 14，原亮蓝带下划线）→ accent（链接 = accent）
    StyleSetForeground(14, pal.accent);
    StyleSetUnderline(14, true);
}

void SigTextEditor::SetJsonStyle() {
    SetLexer(wxSTC_LEX_JSON);

    const UiTheme::Colours& pal = UiTheme::Palette();

    StyleSetBackground(wxSTC_STYLE_DEFAULT, pal.canvas);
    StyleSetForeground(wxSTC_STYLE_DEFAULT, pal.text1);
    StyleClearAll();

    // 属性名 (Key) → accent
    StyleSetForeground(wxSTC_JSON_PROPERTYNAME, pal.accent);

    // 字符串 (Value) → st = warn (CMP-053)
    StyleSetForeground(wxSTC_JSON_STRING, pal.warn);

    // 数值 → 绿族 codeType（SPEC §3.6）
    StyleSetForeground(wxSTC_JSON_NUMBER, pal.codeType);

    // 关键字 (true, false, null) → kw = accent (CMP-053)
    StyleSetForeground(wxSTC_JSON_KEYWORD, pal.accent);
    SetKeyWords(0, "true false null");

    // 操作符 → 正文墨
    StyleSetForeground(wxSTC_JSON_OPERATOR, pal.text1);

    // 错误处理 → danger
    StyleSetForeground(wxSTC_JSON_ERROR, pal.danger);
}


bool SigTextEditor::SaveIfModified() {
    // GetModify() 是 wxSTC 的内置方法，如果缓冲区有变化则返回 true
    if (!this->GetModify()) {
        return true; // 没有修改，直接通过
    }

    wxString msg = wxString::Format("File has been modified. Save changes?");

    wxMessageDialog dlg(this, msg, "Save Confirmation",
        wxYES_NO | wxCANCEL | wxICON_QUESTION);

    int result = dlg.ShowModal();

    if (result == wxID_YES) {
        // 执行保存逻辑（假设你已有 SaveCurrentFile 函数）
        return this->SaveFile();
    }
    else if (result == wxID_NO) {
        return true; // 用户明确不保存，允许覆盖/关闭
    }
    else {
        return false; // 用户点击取消，阻止后续操作
    }
}

void SigTextEditor::OnTextChanged(wxStyledTextEvent& event) {
    // 1. 获取变更类型的位掩码
    int type = event.GetModificationType();

    // 2. 检查是否涉及内容的实质性改变
    // wxSTC_MOD_INSERTTEXT: 插入了文字
    // wxSTC_MOD_DELETETEXT: 删除了文字
    bool isContentChanged = (type & wxSTC_MOD_INSERTTEXT) || (type & wxSTC_MOD_DELETETEXT);

    // 3. 过滤逻辑
    if (!m_isLoading && isContentChanged) {
        this->temp_version++;
        NotifyHeaderState(false);
        // 这里可以执行你的逻辑，比如通知 AsyncAnalysisCenter
    }

    event.Skip();
}

void SigTextEditor::OnDocumentChanged(wxStyledTextEvent& event) {
    // Keep the presentation state honest even when another feature consumes a
    // low-level wxEVT_STC_MODIFIED notification.
    if (!m_isLoading) {
        NotifyHeaderState(false);
    }
    event.Skip();
}

bool SigTextEditor::OpenFile(wxString path) {
    if (!wxFileExists(path)) return false;

    // 1. 物理加载前先清理旧状态
    //this->ClearAllDiagnostics();
    if (!this->SaveIfModified()) {
        return false; // 用户点击了“取消”，终止打开新文件的流程
    }

    m_isLoading = true;

    // 2. 加载文件内容
    if (!this->LoadFile(path)) {
        // OpenFile 失败也要退出加载模式，否则后续真实编辑会被当成加载过程忽略。
        m_isLoading = false;
        return false;
    }

    // 3. 更新当前路径成员
    this->m_currentFilePath = path;

    // 4. 文件类型自动检测
    wxFileName fn(path);
    wxString ext = fn.GetExt().Lower();

    EDITOR_MODE mode = Text; // 默认回退到纯文本
    if (ext == "v" || ext == "sv" || ext == "vh") {
        mode = Verilog;
    }
    else if (ext == "md" || ext == "markdown") {
        mode = Markdown;
    }
    else if (ext == "json" || ext == "project") {
        mode = JsonStyle;
    }

    // 5. 调用模式分发
    this->SetMode(mode);

    // 6. 编辑器环境初始化
    this->EmptyUndoBuffer();
    this->SetSavePoint();

    this->SetToolTip(m_currentFilePath);
    temp_version = 0;
    m_isLoading = false;
    NotifyHeaderState(false);

    return true;
}

void SigTextEditor::ClearDocument()
{
    // 工程切换使用此函数建立“无文件、无修改”的干净编辑器状态。
    m_isLoading = true;
    ClearAll();
    EmptyUndoBuffer();
    SetSavePoint();
    m_currentFilePath.Clear();
    SetToolTip(wxEmptyString);
    temp_version = 0;
    m_isLoading = false;
    NotifyHeaderState(false);
}


bool SigTextEditor::SaveFile() {
    // wxStyledTextCtrl 无路径保存会静默失败，因此先明确拒绝。
    if (m_currentFilePath.IsEmpty()) return false;
    if (!wxStyledTextCtrl::SaveFile(m_currentFilePath)) return false;
    temp_version = 0;
    SetSavePoint();
    return true;
}

bool SigTextEditor::SaveFileAs(wxString path) {
    if (path.IsEmpty()) return false;

    wxFileName fn(path);

    wxString dir = fn.GetPath();
    if (!wxDirExists(dir)) {
        if (!wxFileName::Mkdir(dir, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL)) {
            wxLogError(wxT("无法创建缓存目录: %s"), dir);
            return false;
        }
    }

    if (this->wxStyledTextCtrl::SaveFile(path)) {
        m_currentFilePath = path;
        temp_version = 0;
        SetSavePoint();
        return true;
        
    }
    else 
        return false;
    
}

//void SigTextEditor::OnMouseDwell(wxStyledTextEvent& event) {
//    int pos = event.GetPosition();
//    if (pos == wxSTC_INVALID_POSITION) {
//        if (CallTipActive()) CallTipCancel();
//        return;
//    }
//
//    // 碰撞检测：遍历最新分析结果中的所有消息
//    for (const auto& msg : m_latestAnalysis.lintMessages) {
//        long errStart = PositionFromLine(msg.line - 1) + (msg.col - 1);
//        long errEnd = errStart + (msg.length > 0 ? msg.length : 3);
//
//        // 如果鼠标位置在波浪线范围内 (容错范围 +2)
//        if (pos >= errStart && pos <= errEnd + 2) {
//            this->ShowReactiveDetail(pos, msg);
//            return;
//        }
//    }
//
//    // 如果没指着任何错误，取消旧提示
//    if (CallTipActive()) CallTipCancel();
//}
//
//void SigTextEditor::ShowReactiveDetail(int pos, const LintMessage& msg) {
//    // 格式化输出：[来源] 错误代码: 描述
//    wxString sourceName = (msg.source == LintMessage::Source::SLANG) ? "Slang" : "Syntax";
//    wxString info = wxString::Format(" %s | %s: %s ", sourceName, msg.code, msg.desc);
//
//    if (!CallTipActive()) {
//        CallTipShow(pos, info);
//    }
//}








//bool SigTextEditor::VisualFeedBack(const LintRes& res) {
//    this->m_latestAnalysis = res;
//    this->Freeze();
//
//    // 1. 渲染诊断波浪线 (Indicators)
//    //this->RenderDiagnosticIndicators(res.lintMessages);
//
//    // 2. 渲染物理结构 (只有 Tree-sitter 成功时)
//    if (res.ts_success) {
//        //this->RenderBlockHealthStatus(res.blocks);
//        //this->RenderFoldingStructure(res.blocks);
//    }
//
//    // 3. 渲染语义高亮 (只有 Slang 成功时)
//    if (res.slang_success) {
//        //this->RenderSemanticColors(res.blocks);
//    }
//
//    this->Thaw();
//    return true;
//}

// --- 依赖函数实现 ---


//void SigTextEditor::RenderDiagnosticIndicators(const std::vector<LintMessage>& msgs) {
//    // 清除旧的波浪线
//    this->SetIndicatorCurrent(INDIC_ERROR);
//    this->IndicatorClearRange(0, this->GetLength());
//    this->SetIndicatorCurrent(INDIC_WARNING);
//    this->IndicatorClearRange(0, this->GetLength());
//
//    for (const auto& msg : msgs) {
//        int indicId = msg.isError ? INDIC_ERROR : INDIC_WARNING;
//        this->SetIndicatorCurrent(indicId);
//
//        // 计算精确位置
//        long pos = this->PositionFromLine(msg.line - 1) + (msg.col - 1);
//        this->IndicatorFillRange(pos, msg.length > 0 ? msg.length : 1);
//    }
//}
//
//void SigTextEditor::RenderFoldingStructure(const std::vector<VerilogBlock>& blocks) {
//    // 重置所有折叠层级
//    for (int i = 0; i < GetLineCount(); ++i) {
//        this->SetFoldLevel(i, wxSTC_FOLDLEVELBASE);
//    }
//
//    for (const auto& block : blocks) {
//        if (block.end_line > block.start_line) {
//            int level = wxSTC_FOLDLEVELBASE + block.nesting_level;
//
//            // 标记 Header 行
//            this->SetFoldLevel(block.start_line - 1, level | wxSTC_FOLDLEVELHEADERFLAG);
//
//            // 填充后续行
//            for (int l = block.start_line; l < block.end_line; ++l) {
//                if (l < GetLineCount()) {
//                    this->SetFoldLevel(l, level);
//                }
//            }
//        }
//    }
//}
//
//void SigTextEditor::RenderSemanticColors(const std::vector<VerilogBlock>& blocks) {
//    // 语义着色的关键在于：不破坏基础 Style，而是针对特定 Range 覆盖颜色
//    // 注意：wxSTC 默认高亮后，手动着色需要谨慎处理
//
//    //for (const auto& block : blocks) {
//    //    // 这里的 atoms 或 SemanticInfo 需要在 Slang 阶段被填充
//    //    // 每个 Atom 包含：{name, position, length, type}
//    //    for (const auto& atom : block.semantic_info.atoms) {
//    //        // 1. 设置渲染起点
//    //        this->StartStyling(atom.position);
//
//    //        // 2. 根据符号类型应用样式
//    //        int styleId = STYLE_DEFAULT;
//    //        if (atom.type == "port") styleId = STYLE_SEMANTIC_PORT;
//    //        else if (atom.type == "parameter") styleId = STYLE_SEMANTIC_PARAM;
//    //        else if (atom.type == "instance") styleId = STYLE_SEMANTIC_INST;
//
//    //        // 3. 应用样式
//    //        this->SetStyling(atom.length, styleId);
//    //    }
//    //}
//}
//
//void SigTextEditor::ClearAllDiagnostics() {
//    this->Freeze();
//
//    // 1. 清除波浪线 (Indicators)
//    this->SetIndicatorCurrent(INDIC_ERROR);
//    this->IndicatorClearRange(0, this->GetLength());
//    this->SetIndicatorCurrent(INDIC_WARNING);
//    this->IndicatorClearRange(0, this->GetLength());
//
//    // 2. 清除侧边栏状态条 (Markers)
//    this->MarkerDeleteAll(MARKER_ID_STABLE);
//    this->MarkerDeleteAll(MARKER_ID_CORRUPTED);
//    this->MarkerDeleteAll(MARKER_ID_INCOMPLETE);
//
//    // 3. 清除折叠结构 (重置为基础层级)
//    for (int i = 0; i < GetLineCount(); ++i) {
//        this->SetFoldLevel(i, wxSTC_FOLDLEVELBASE);
//    }
//
//    // 4. 清除 CallTip (如果存在)
//    if (this->CallTipActive()) {
//        this->CallTipCancel();
//    }
//
//    // 5. 重置数据快照
//    this->m_latestAnalysis = LintRes();
//
//    this->Thaw();
//}

bool SigTextEditor::VisualFeedBack(const LintResult res) {
    RenderLineMarker(res.line_status);
    RenderFoldingStructure(res.is_lines_header, res.line_depth);
    return true;
}

void SigTextEditor::RenderLineMarker(const std::vector<Stability> line_status) {

    // 1. 清除当前编辑器中所有的旧标记 (假设是在第 0 号 Margin)
    int lineCount = GetLineCount();
    for (int i = 0; i < lineCount; i++) {
        MarkerDelete(i, MARKER_ID_STABLE);
        MarkerDelete(i, MARKER_ID_CORRUPTED);
        MarkerDelete(i, MARKER_ID_INCOMPLETE);
    }



    // 2. 遍历状态数组 (从索引 1 开始)
    for (size_t i = 0; i < line_status.size(); ++i) {
        int stc_line_number = static_cast<int>(i); // 转换为 0-based

        switch (line_status[i]) {
        case Stability::Stable:
            MarkerAdd(stc_line_number, MARKER_ID_STABLE);
            break;
        case Stability::Corrupted:
            MarkerAdd(stc_line_number, MARKER_ID_CORRUPTED);
            break;
        case Stability::Incomplete:
            MarkerAdd(stc_line_number, MARKER_ID_INCOMPLETE);
            break;
        default:
            // 未知状态不渲染或渲染默认样式
            break;
        }
    }
}

void SigTextEditor::MarkerDeleteAllByID(int markerId) {
    // 获取当前编辑器的总行数
    int lineCount = GetLineCount();

    for (int i = 0; i < lineCount; ++i) {
        // 检查第 i 行是否包含这个特定的 markerId
        // MarkerGet 返回的是一个位掩码 (Bitmask)
        if (MarkerGet(i) & (1 << markerId)) {
            MarkerDelete(i, markerId);
        }
    }
}

void SigTextEditor::RenderFoldingStructure(const std::vector<bool> is_lines_header, const std::vector<int> line_depth) {
    int total_lines = line_depth.size()-1;
    Freeze();

    for (int i = 1; i <= total_lines; ++i) {
        int level = wxSTC_FOLDLEVELBASE + line_depth[i];

        if (is_lines_header[i]) {
            level |= wxSTC_FOLDLEVELHEADERFLAG;
        }

        SetFoldLevel(i - 1, level);
    }
    DebugFoldLevels();
    Thaw();

}

void SigTextEditor::OnMarginClick(wxStyledTextEvent& event) {
    // 假设你的折叠标记在 Margin 2 (这是通用的做法)
    if (event.GetMargin() == 2) {
        // 1. 获取点击位置对应的行号
        int lineClick = LineFromPosition(event.GetPosition());

        // 2. 获取该行的折叠层级
        int levelClick = GetFoldLevel(lineClick);

        // 3. 判断这一行是不是 Header（有没有小方框）
        if (levelClick & wxSTC_FOLDLEVELHEADERFLAG) {
            // 4. 执行折叠或展开切换
            ToggleFold(lineClick);
        }
    }
}

void SigTextEditor::DebugFoldLevels() {
    int total_lines = GetLineCount();
    wxLogDebug("--- Folding Debug Info (Detailed) ---");

    for (int i = 0; i < total_lines; ++i) {
        // 1. 获取原始的 Level 信息
        int level_raw = GetFoldLevel(i);

        // 2. 提取层级数字 (屏蔽掉所有 Flag)
        int level_num = level_raw & wxSTC_FOLDLEVELNUMBERMASK;

        // 3. 判断是否为 Header
        bool is_header = (level_raw & wxSTC_FOLDLEVELHEADERFLAG) != 0;

        // 4. 获取该行的折叠展开状态 (API 调用)
        bool is_expanded = GetFoldExpanded(i);

        // 5. 获取该行的父级行号 (API 调用)
        // 这个非常有用！Scintilla 会自动告诉你这一行归谁管
        int parent_line = GetFoldParent(i);

        // 6. 构造可视化缩进
        wxString indent = "";
        int depth = level_num - wxSTC_FOLDLEVELBASE;
        for (int j = 0; j < depth; ++j) indent += "  ";

        // 7. 格式化输出
        // 标记位说明：H (Header), E (Expanded)
        wxString info = wxString::Format(
            "Line %3d | [0x%04X] | L:%d | Parent:%3d | %s%s %s",
            i + 1,
            level_raw,
            level_num,
            parent_line + 1, // 转换为 1-based
            indent,
            is_header ? (is_expanded ? "[-] " : "[+] ") : "  | ",
            is_header ? "<-- HEADER" : ""
        );

        wxLogDebug(info);
    }
    wxLogDebug("-------------------------------------");
}

