#include "SigTextEditor.h"
#include <wx/filename.h>

SigTextEditor::SigTextEditor(wxWindow* parent)
    : wxStyledTextCtrl(parent, wxID_ANY) {
    SetMode(EDITOR_MODE::Verilog);
    SetCodePage(wxSTC_CP_UTF8);
    SetProperty("fold.compact", "0"); // 不要压缩空行折叠
    SetProperty("fold.comment", "1"); // 如果有注释折叠可以开启
    Bind(wxEVT_STC_MARGINCLICK, &SigTextEditor::OnMarginClick, this);
    Bind(wxEVT_STC_CHANGE, &SigTextEditor::OnTextChanged, this);
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

    
}

void SigTextEditor::SetVerilogStyle() {
    SetLexer(wxSTC_LEX_VERILOG);

    // 1. 基础全局样式 (白色背景，黑色字)
    wxFont codeFont(12, wxFONTFAMILY_TELETYPE, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_NORMAL);
    StyleSetFont(wxSTC_STYLE_DEFAULT, codeFont);
    StyleSetBackground(wxSTC_STYLE_DEFAULT, wxColour(255, 255, 255)); // 纯白背景
    StyleSetForeground(wxSTC_STYLE_DEFAULT, wxColour(30, 30, 30));    // 深黑文字
    StyleClearAll();

    // 2. 边缘设置 (行号区域)
    SetMarginType(0, wxSTC_MARGIN_NUMBER);
    SetMarginWidth(0, 45);
    StyleSetBackground(wxSTC_STYLE_LINENUMBER, wxColour(240, 240, 240)); // 浅灰行号背景
    StyleSetForeground(wxSTC_STYLE_LINENUMBER, wxColour(100, 100, 100)); // 灰色数字
    SetMarginMask(0, 0);

    // 3. 配置关键字 (保持不变)
    SetKeyWords(0, "module endmodule input output inout wire reg assign "
        "always initial begin end if else case endcase parameter "
        "localparam generate endgenerate posedge negedge or "
        "integer genvar function endfunction task endtask "
        "default for while repeat forever wait");
    SetKeyWords(1, "$display $monitor $write $finish $stop $random");

    // 4. 应用浅色模式语法高亮
    // 关键字 - 蓝色 (经典 IDE 风格)
    StyleSetForeground(wxSTC_V_WORD, wxColour(0, 0, 255));
    StyleSetBold(wxSTC_V_WORD, false); // 白色背景下通常不需要太粗

    // 系统任务 ($) - 紫色或洋红色
    StyleSetForeground(wxSTC_V_WORD2, wxColour(175, 0, 219));

    // 注释 - 绿色 (经典的森林绿)
    StyleSetForeground(wxSTC_V_COMMENT, wxColour(0, 128, 0));
    StyleSetForeground(wxSTC_V_COMMENTLINE, wxColour(0, 128, 0));

    // 字符串 - 棕红色
    StyleSetForeground(wxSTC_V_STRING, wxColour(163, 21, 21));

    // 数字 - 蓝绿色或保持黑色
    StyleSetForeground(wxSTC_V_NUMBER, wxColour(9, 134, 88));

    // 操作符 - 黑色
    StyleSetForeground(wxSTC_V_OPERATOR, wxColour(0, 0, 0));

    // 预处理/宏 - 紫褐色
    StyleSetForeground(wxSTC_V_PREPROCESSOR, wxColour(100, 40, 100));

    // 5. 编辑器 UI 细节
    SetCaretForeground(wxColour(0, 0, 0));            // 黑色光标
    SetSelBackground(true, wxColour(173, 214, 255)); // 浅蓝色选中区

    // 当前行高亮（浅灰色条）
    SetCaretLineVisible(true);
    SetCaretLineBackground(wxColour(245, 245, 245));

    StyleSetForeground(wxSTC_STYLE_BRACELIGHT, wxColour(255, 0, 0)); // 红色加粗
    StyleSetBold(wxSTC_STYLE_BRACELIGHT, true);
}

void SigTextEditor::SetVerilogIDE() {
    // --- A. Margin 1: 逻辑块健康状态条 ---
    SetMarginWidth(1, 10);
    SetMarginType(1, wxSTC_MARGIN_SYMBOL);
    SetMarginSensitive(1, false);
    SetMarginMask(1, 0xFFFFFFFF);
    SetMarginMask(1, ~wxSTC_MASK_FOLDERS);

    // Marker 1: 稳定态 (绿色)
    MarkerDefine(1, wxSTC_MARK_FULLRECT);
    MarkerSetForeground(1, wxColour(100, 200, 100));
    MarkerSetBackground(1, wxColour(100, 200, 100));

    // Marker 2: 错误态 (红色)
    MarkerDefine(2, wxSTC_MARK_FULLRECT);
    MarkerSetForeground(2, wxColour(220, 50, 50));
    MarkerSetBackground(2, wxColour(220, 50, 50));

    // Marker 3: 未完成 (黄色)
    MarkerDefine(3, wxSTC_MARK_FULLRECT);
    MarkerSetForeground(3, wxColour(240, 200, 50));
    MarkerSetBackground(3, wxColour(240, 200, 50));

    // --- B. Margin 2: 代码折叠槽 ---
    SetMarginType(2, wxSTC_MARGIN_SYMBOL);
    SetMarginMask(2, wxSTC_MASK_FOLDERS);
    SetMarginWidth(2, 16);
    SetMarginSensitive(2, true);
    SetProperty("fold", "1"); // 激活 Scintilla 折叠引擎

    // 定义经典折叠图标 (加减号)
    //MarkerDefine(wxSTC_MARKNUM_FOLDEROPEN, wxSTC_MARK_BOXMINUS, *wxWHITE, wxColour(120, 120, 120));
    //MarkerDefine(wxSTC_MARKNUM_FOLDER, wxSTC_MARK_BOXPLUS, *wxWHITE, wxColour(120, 120, 120));
    //MarkerDefine(wxSTC_MARKNUM_FOLDERSUB, wxSTC_MARK_VLINE, *wxWHITE, wxColour(120, 120, 120));
    //MarkerDefine(wxSTC_MARKNUM_FOLDERTAIL, wxSTC_MARK_LCORNER, *wxWHITE, wxColour(120, 120, 120));
    //MarkerDefine(wxSTC_MARKNUM_FOLDEREND, wxSTC_MARK_LCORNER, *wxWHITE, wxColour(120, 120, 120));
    SetFoldFlags(wxSTC_FOLDFLAG_LINEBEFORE_CONTRACTED | wxSTC_FOLDFLAG_LINEAFTER_CONTRACTED);

    
    
    
    // 【1】自定义：未折叠状态的头（方框减号）
    MarkerDefine(wxSTC_MARKNUM_FOLDEROPEN, wxSTC_MARK_BOXMINUS, *wxWHITE, wxColour(120, 120, 120));

    // 【2】自定义：已折叠状态的头（方框加号）
    MarkerDefine(wxSTC_MARKNUM_FOLDER, wxSTC_MARK_BOXPLUS, *wxWHITE, wxColour(120, 120, 120));

    // 【3】自定义：折叠块中间的垂直连线
    MarkerDefine(wxSTC_MARKNUM_FOLDERSUB, wxSTC_MARK_VLINE, *wxWHITE, wxColour(120, 120, 120));

    // 【4】自定义：展开状态下，块结尾的那个 L 型拐角
    MarkerDefine(wxSTC_MARKNUM_FOLDERTAIL, wxSTC_MARK_LCORNER, *wxWHITE, wxColour(120, 120, 120));

    // 【5】自定义：折叠状态下，如果下面还有嵌套，结尾显示的那个带连线的方框（极其重要！）
    // 这就是解决你“二级 Header 变圆圈”的关键
    MarkerDefine(wxSTC_MARKNUM_FOLDEREND, wxSTC_MARK_BOXPLUSCONNECTED, *wxWHITE, wxColour(120, 120, 120));

    // 【6】自定义：处于打开状态的子级头（带连线的减号方框）
    MarkerDefine(wxSTC_MARKNUM_FOLDEROPENMID, wxSTC_MARK_BOXMINUSCONNECTED, *wxWHITE, wxColour(120, 120, 120));

    // 【7】自定义：由于某些跳变产生的中间连线
    MarkerDefine(wxSTC_MARKNUM_FOLDERMIDTAIL, wxSTC_MARK_VLINE, *wxWHITE, wxColour(120, 120, 120));
    
    
    
    
    
    
    
    
    
    
    
    
    // --- C. 诊断波浪线配置 (Indicators) ---
    IndicatorSetStyle(0, wxSTC_INDIC_SQUIGGLE); // Error
    IndicatorSetForeground(0, wxColour(220, 50, 50));
    IndicatorSetStyle(1, wxSTC_INDIC_SQUIGGLE); // Warning
    IndicatorSetForeground(1, wxColour(255, 150, 0));

    // --- D. 语义高亮样式定义 (为后续渲染准备) ---
    // 定义常量（建议在类成员或头文件中统一管理）
#define STYLE_SEM_PORT 20
#define STYLE_SEM_PARAM 21
#define STYLE_SEM_INST 22
    StyleSetForeground(STYLE_SEM_PORT, wxColour(255, 128, 0));  // 橙色端口
    StyleSetBold(STYLE_SEM_PORT, true);
    StyleSetForeground(STYLE_SEM_PARAM, wxColour(128, 0, 255)); // 紫色参数
    StyleSetForeground(STYLE_SEM_INST, wxColour(0, 128, 192));  // 蓝绿实例

    // --- E. 交互反馈 ---
    SetMouseDwellTime(500); // 开启 500ms 悬停检测
    SetCaretForeground(wxColour(0, 0, 0));
    SetCaretLineVisible(true);
    SetCaretLineBackground(wxColour(245, 245, 245));
    SetSelBackground(true, wxColour(173, 214, 255));
}
void SigTextEditor::SetTextStyle() {
    ClearAll();
    SetLexer(wxSTC_LEX_NULL);

    // 默认字体：深灰色文字，纯白背景
    wxFont font(12, wxFONTFAMILY_TELETYPE, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_NORMAL);
    StyleSetFont(wxSTC_STYLE_DEFAULT, font);
    StyleSetBackground(wxSTC_STYLE_DEFAULT, wxColour(255, 255, 255)); // 纯白
    StyleSetForeground(wxSTC_STYLE_DEFAULT, wxColour(30, 30, 30));    // 近乎黑色

    StyleClearAll();

    // 配置行号列 (Margin 0)：浅灰色背景，深灰色数字
    SetMarginType(0, wxSTC_MARGIN_NUMBER);
    SetMarginWidth(0, 45);
    StyleSetForeground(wxSTC_STYLE_LINENUMBER, wxColour(120, 120, 120));
    StyleSetBackground(wxSTC_STYLE_LINENUMBER, wxColour(240, 240, 240));

    // 设置光标为黑色
    SetCaretForeground(wxColour(0, 0, 0));
}

void SigTextEditor::SetMarkdownStyle() {
    SetLexer(wxSTC_LEX_MARKDOWN);

    // 基础继承
    StyleSetBackground(wxSTC_STYLE_DEFAULT, wxColour(255, 255, 255));
    StyleSetForeground(wxSTC_STYLE_DEFAULT, wxColour(30, 30, 30));
    StyleClearAll();

    // 标题 (Headers) - 深蓝色
    StyleSetForeground(1, wxColour(0, 56, 121));  // H1
    StyleSetBold(1, true);
    StyleSetForeground(2, wxColour(0, 56, 121));  // H2
    StyleSetForeground(3, wxColour(0, 56, 121));  // H3

    // 代码块 - 棕红色
    StyleSetForeground(19, wxColour(163, 21, 21));
    StyleSetForeground(20, wxColour(163, 21, 21));

    // 列表 - 深绿色
    StyleSetForeground(15, wxColour(0, 128, 0));

    // 粗体/斜体
    StyleSetBold(10, true);
    StyleSetItalic(11, true);

    // 链接 - 亮蓝色带下划线
    StyleSetForeground(14, wxColour(0, 0, 255));
    StyleSetUnderline(14, true);
}

void SigTextEditor::SetJsonStyle() {
    SetLexer(wxSTC_LEX_JSON);

    StyleSetBackground(wxSTC_STYLE_DEFAULT, wxColour(255, 255, 255));
    StyleSetForeground(wxSTC_STYLE_DEFAULT, wxColour(30, 30, 30));
    StyleClearAll();

    // 属性名 (Key) - 深蓝色 (VS 经典色)
    StyleSetForeground(wxSTC_JSON_PROPERTYNAME, wxColour(4, 81, 165));

    // 字符串 (Value) - 棕红色
    StyleSetForeground(wxSTC_JSON_STRING, wxColour(163, 21, 21));

    // 数值 - 翠绿色
    StyleSetForeground(wxSTC_JSON_NUMBER, wxColour(9, 134, 88));

    // 关键字 (true, false, null) - 亮蓝色
    StyleSetForeground(wxSTC_JSON_KEYWORD, wxColour(0, 0, 255));
    SetKeyWords(0, "true false null");

    // 操作符 - 黑色
    StyleSetForeground(wxSTC_JSON_OPERATOR, wxColour(0, 0, 0));

    // 错误处理 - 亮红色
    StyleSetForeground(wxSTC_JSON_ERROR, wxColour(200, 0, 0));
}


bool SigTextEditor::SaveIfModified() {
    // GetModify() 是 wxSTC 的内置方法，如果缓冲区有变化则返回 true
    if (!this->GetModify()) {
        return true; // 没有修改，直接通过
    }

    wxString msg = wxString::Format("File has been modified. Save changes?");

    // 弹出标准对话框：是、否、取消
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
    int type = event.GetModificationType();

    // 只在 插入文本、删除文本、撤销、重做 时增加版本
    if (m_isLoading && (type & (wxSTC_MOD_INSERTTEXT | wxSTC_MOD_DELETETEXT |
        wxSTC_PERFORMED_UNDO | wxSTC_PERFORMED_REDO))) {
        this->temp_version++;
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
    if (!this->LoadFile(path)) return false;

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

    return true;
}


bool SigTextEditor::SaveFile() {
    temp_version = 0;
    return wxStyledTextCtrl::SaveFile(m_currentFilePath);
}

bool SigTextEditor::SaveFileAs(wxString path) {
    if (path.IsEmpty()) return false;

    wxFileName fn(path);

    wxString dir = fn.GetPath();
    if (!wxDirExists(dir)) {
        if (!wxFileName::Mkdir(dir, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL)) {
            wxLogError("无法创建缓存目录: %s", dir);
            return false;
        }
    }

    if (this->wxStyledTextCtrl::SaveFile(path)) {
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
    for (size_t i = 1; i < line_status.size(); ++i) {
        int stc_line_number = static_cast<int>(i) - 1; // 转换为 0-based

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

