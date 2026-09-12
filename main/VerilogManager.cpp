#include "VerilogManager.h"
#include "SigTree.h"
#include "SigTextEditor.h"
#include "VerilogStructuring.h"
#include <algorithm>
#include <wx/time.h>

VerilogManager::VerilogManager(SigTextEditor* stc, SigFlowTree* tree, TSParser* parser)
    : fn(nullptr), m_stc(stc), m_tree(tree), editing_top_block(nullptr),
      recovering_block(nullptr), m_timer(nullptr), m_ts_tree(nullptr), m_ts_parser(parser) {
    m_timer = new wxTimer();
    m_timer->Bind(
        wxEVT_TIMER,
        &VerilogManager::OnTimer,
        this);
    m_stc->SetModEventMask(
        wxSTC_MOD_INSERTTEXT |
        wxSTC_MOD_DELETETEXT
    );
    m_stc->Bind(
        wxEVT_STC_MODIFIED,
        &VerilogManager::EditBlock,
        this
        );
    wxColour grayText(160, 160, 160); 
    
    // 2. 获取当前编辑器的基础字体，并在其基础上修改
    wxFont annotationFont = m_stc->GetFont(); 
    annotationFont.SetPointSize(annotationFont.GetPointSize() + 5); // 减小 2 号字
    annotationFont.SetStyle(wxFONTSTYLE_ITALIC);                  // 设置为斜体

    // 3. 将属性应用到 Style ID 10
    m_stc->StyleSetFont(50, annotationFont);
    m_stc->StyleSetForeground(50, wxColor(*wxBLUE));
    
    // 4. 背景色：设为与编辑器背景一致，模拟“透明”效果
    // 这样注记就不会有一个突兀的背景方块
    m_stc->StyleSetBackground(50, m_stc->StyleGetBackground(wxSTC_STYLE_DEFAULT));

    // 5. 设置显示模式 (Mode 3: 随代码缩进对齐，最整齐)
    m_stc->AnnotationSetVisible(0);
}

VerilogManager::~VerilogManager() {
    if (m_stc) {
        m_stc->Unbind(wxEVT_STC_MODIFIED, &VerilogManager::EditBlock, this);
    }
    if (m_timer) {
        m_timer->Stop();
        delete m_timer;
        m_timer = nullptr;
    }
    if (m_ts_tree) {
        ts_tree_delete(m_ts_tree);
        m_ts_tree = nullptr;
    }
    // Note: m_ts_parser is owned externally, do not delete
}

void VerilogManager::ClearFileNode()
{
    // 切换工程前丢弃旧文件的解析树、块范围和行标记，避免指向已释放的节点。
    if (m_timer) m_timer->Stop();
    if (m_stc) m_stc->MarkerDeleteAll(BLOCK_MARKER_ID);
    blocks.clear();
    break_blocks.clear();
    structures.clear();
    editing_top_block = nullptr;
    recovering_block = nullptr;
    fn = nullptr;
    if (m_ts_tree) {
        ts_tree_delete(m_ts_tree);
        m_ts_tree = nullptr;
    }
}

void VerilogManager::OnTimer(wxTimerEvent&) {
    if (!m_stc) {
        return;
    }

    auto refreshEditorState = [this]() {
        m_stc->RenderLineMarker(GetLineStatus());
        UpdateFolding();
        m_stc->DebugFoldLevels();
        DrawBlockInfo();
        Print();
    };

    if (!m_tree || !m_ts_parser || !editing_top_block) {
        refreshEditorState();
        return;
    }

    for (Block& b : GetBreakBlocks()) {
        if (!b.structure) {
            continue;
        }

        wxString delta = GetBlockText(b);

        std::string x = "module _tmp;\n" + delta.ToStdString() + "\nendmodule\n";
        TSTree* fragmentTree = ts_parser_parse_string(m_ts_parser, nullptr, x.c_str(), x.length());
        if (fragmentTree) {
            TSNode fragmentRoot = ts_tree_root_node(fragmentTree);
            if (ts_node_has_error(fragmentRoot)) {
                ts_tree_delete(fragmentTree);
                continue;
            }
            // 1. 获取基础数据
            std::string fp = m_stc->m_currentFilePath.ToStdString();
            std::string fullCode = m_stc->GetText().ToStdString();

            if (m_ts_tree) ts_tree_delete(m_ts_tree);
            m_ts_tree = ts_parser_parse_string(m_ts_parser, nullptr, fullCode.c_str(), fullCode.length());
            if (!m_ts_tree) {
                ts_tree_delete(fragmentTree);
                refreshEditorState();
                return;
            }

            TSTreeCursor cursor = ts_tree_cursor_new(fragmentRoot);
            std::unordered_map<SigTreeNode*, std::tuple<int, int>> map;

            // 找到该文件对应的根节点开始同步
            SigTreeNode* fileNode = m_tree->GetFileNode(fp);
            if (fileNode) {
                if (b.self == nullptr && editing_top_block->self) {
                    m_tree->UpdateTreeFromTS(&cursor, editing_top_block->self, fp, x, map);
                }
            }
            ts_tree_cursor_delete(&cursor);
            ts_tree_delete(fragmentTree);

            refreshEditorState();
            return;
        }
    }
    refreshEditorState();
}

bool VerilogManager::SetFileNode(FileNode* n, std::unordered_map<SigTreeNode*, std::tuple<int, int>> map){
    // 成功返回才允许调用者更新界面选择，防止打开失败时“树和编辑器不同步”。
    if (!n || !m_stc || !m_ts_parser) {
        return false;
    }

    if (!m_stc->OpenFile(n->filePath)) {
        return false;
    }

    if (m_timer) m_timer->Stop();
    m_stc->MarkerDeleteAll(BLOCK_MARKER_ID);
    blocks.clear();
    break_blocks.clear();
    structures.clear();
    editing_top_block = nullptr;
    recovering_block = nullptr;

    this->fn = n;

    std::string text = m_stc->GetText().ToStdString();
    if (m_ts_tree) ts_tree_delete(m_ts_tree);
    m_ts_tree = ts_parser_parse_string(m_ts_parser, nullptr, text.c_str(), text.length());
    if (!m_ts_tree) {
        return true;
    }
    CollectBlocks(map);
    m_stc->RenderLineMarker(GetLineStatus());
    UpdateFolding();
    DrawBlockInfo();
    return true;
}


void VerilogManager::CollectBlocks(std::unordered_map<SigTreeNode*, std::tuple<int, int>>& map) {
    // 1. 清理旧的标记，防止多次解析后句柄堆积
    m_stc->MarkerDeleteAll(BLOCK_MARKER_ID);
    blocks.clear();
    break_blocks.clear();
    structures.clear();
    editing_top_block = nullptr;
    recovering_block = nullptr;

    // 2. 为了保持 Print() 时的顺序性，建议先将 map 元素放入 vector 排序
    // 如果不关心顺序，可以直接执行第 3 步的循环
    std::vector<std::pair<SigTreeNode*, std::tuple<int, int>>> sortedItems(map.begin(), map.end());

    std::sort(sortedItems.begin(), sortedItems.end(), [](const auto& a, const auto& b) {
        return std::get<0>(a.second) < std::get<0>(b.second);
        });

    // 3. 遍历并添加块
    int maxLine = m_stc->GetLineCount() - 1;

    for (const auto& item : sortedItems) {
        SigTreeNode* node = item.first;
        auto [startLine, endLine] = item.second;

        // 边界保护：确保行号不会超出当前编辑器的最大行
        // 尤其是 FileNode 的 endLine 往往是文件总行数，会导致 MarkerAdd 失败
        int safeStart = std::clamp(startLine, 0, maxLine);
        int safeEnd = std::clamp(endLine, 0, maxLine);

        // 调用 AddBlock
        this->AddBlock(safeStart, safeEnd, node);
    }
}

void VerilogManager::AppendBlocks(std::unordered_map<SigTreeNode*, std::tuple<int, int>>& map) {
    // 1. 获取当前编辑器的最大行数，用于边界保护
    int maxLine = m_stc->GetLineCount() - 1;
    if (maxLine < 0) maxLine = 0;

    // 2. 遍历传入的 map
    for (auto const& [node, range] : map) {
        // 提取起始行和结束行
        int startLine = std::get<0>(range);
        int endLine = std::get<1>(range);

        // 3. 边界修正：确保 Tree-sitter 返回的行号在 Scintilla 合法范围内
        // 尤其是处理文件末尾节点时，TS 常返回 LineCount，需修正为 LineCount - 1
        int safeStart = std::clamp(startLine, 0, maxLine);
        int safeEnd = std::clamp(endLine, 0, maxLine);

        // 4. 调用 AddBlock 将其转化为句柄并存入 blocks 向量
        // AddBlock 内部会执行 m_stc->MarkerAdd
        this->AddBlock(safeStart, safeEnd, node);
    }

    // 选做：如果需要保持全局块的有序性，可以在此处进行一次排序
    
    blocks.sort([this](const Block& a, const Block& b) {
        return m_stc->MarkerLineFromHandle(a.startHandle) < m_stc->MarkerLineFromHandle(b.startHandle);
    });
    
}


void VerilogManager::AddBlock(int startline, int endline, SigTreeNode* node) {
    int s = m_stc->MarkerAdd(startline, BLOCK_MARKER_ID);
    int e = m_stc->MarkerAdd(endline, BLOCK_MARKER_ID);
    blocks.push_back({ s, e, node});
}

Block* VerilogManager::AddBreakBlock(int startline, int endline) {
    int s = m_stc->MarkerAdd(startline, BLOCK_MARKER_ID);
    int e = m_stc->MarkerAdd(endline, BLOCK_MARKER_ID);
    break_blocks.emplace_back(s, e, nullptr);
    structures.emplace_back();
    Structuring& structure = structures.back();
    Block* b = &break_blocks.back();
    int blockStartLine = GetSTCLine(b->startHandle);
    int pos = blockStartLine >= 0 ? m_stc->PositionFromLine(blockStartLine) : 0;
    wxString text = GetBlockText(*b);
    structure.OnInsert(pos, text.ToStdString());
    b->structure = &structure;
    return b;
}

void VerilogManager::RecoverBreakBlock(Block* b, SigTreeNode* n) {
    if (!b) return;
    auto it = std::find_if(break_blocks.begin(), break_blocks.end(), [b](const Block& item) {
        return &item == b;
        });

    if (it != break_blocks.end()) {
        if (b->structure) {
            b->structure->Clear();
        }
        b->structure = nullptr;
        it->self = n; // 将解析出的新语义节点赋值给 self

        // 3. 将其移入稳定集合 blocks
        blocks.splice(blocks.end(), break_blocks, it);
        //m_tree->AddChild(n->GetParent(), n); // 将新节点挂回树结构
    }
    recovering_block = nullptr;
    editing_top_block = nullptr;
}


Block* VerilogManager::FindBlock(int start, int end) {
    int offset = 1;
    // 1. 定义一个 lambda 用于检查行号覆盖逻辑
    auto isCovered = [this, start, end, offset](Block& b) -> bool {
        int bStart = this->GetLine(b.startHandle);
        int bEnd = this->GetLine(b.endHandle);
        
        // 覆盖逻辑：传入的范围 [start, end] 是否在 block 的 [bStart, bEnd] 内部
        // 或者：该 block 是否包含传入的起始行 (根据你的业务逻辑调整)
        if (b.isStable()) {
            if (b.self->type == SigTreeNodeType::Top) {
                editing_top_block = &b;
                return false;
            }
            else if(b.self->type == SigTreeNodeType::File) {
                return false;
            }
        }
        return (start+ offset >= bStart && end+ offset <= bEnd);
    };

    // 2. 在稳定块 (blocks) 中查找
    for (auto& b : blocks) {
        if (isCovered(b)) {
            return &b;
        }
    }

    // 3. 在断裂块 (break_blocks) 中查找
    for (auto& b : break_blocks) {
        if (isCovered(b)) {
            return &b;
        }
    }

    // 4. 未找到匹配项
    return nullptr;
}

 void VerilogManager::EditBlock(wxStyledTextEvent& event) {
     if (m_stc->m_isLoading) {
         m_timer->Start(300, wxTIMER_ONE_SHOT);
         return;
    }
    int type = event.GetModificationType();
    int pos = event.GetPosition();
    int len = event.GetLength();
    wxString text = event.GetText();

    int start = m_stc->LineFromPosition(pos);
    int end = m_stc->LineFromPosition(pos + static_cast<int>(text.size()));
    Block* b = FindBlock(start, end);
    bool initializedFromCurrentText = false;
    if (!b) {
        b = AddBreakBlock(start, end);
        initializedFromCurrentText = true;
    }

    if (b->isStable()) {
        auto it = std::find_if(blocks.begin(), blocks.end(), [b](const Block& item) {
            return &item == b; // 通过地址匹配
            });

        if (it != blocks.end()) {
            SigTreeNode* n = it->self;
            it->self = nullptr;
            int startLine = GetSTCLine(it->startHandle);
            int startPos = startLine >= 0 ? m_stc->PositionFromLine(startLine) : 0;

            // 2. 获取结束行的行尾位置
            // 注意：第 N 行的行尾，实际上就是第 N+1 行的行首

            // 3. 此时获取的才是完整的跨行文本
            wxString t = GetBlockText(*it);
            structures.emplace_back();
            Structuring& structure = structures.back();
            structure.OnInsert(startPos, t.ToStdString());
            it->structure = &structure;
            break_blocks.splice(break_blocks.end(), blocks, it);
            initializedFromCurrentText = true;
            if (m_tree && n && n->GetParent()) {
                m_tree->RemoveChild(n->GetParent(), n);
            }
        }
    }

    Structuring* sp = b->structure;
    if (!sp) {
        return;
    }
    recovering_block = b;
    if (!initializedFromCurrentText) {
        if (type & wxSTC_MOD_INSERTTEXT)
        {
            sp->OnInsert(pos, text.ToStdString());
        }

        if (type & wxSTC_MOD_DELETETEXT)
        {
            sp->OnDelete(pos, len);
        }
    }
    m_timer->Start(300, wxTIMER_ONE_SHOT);
}




int VerilogManager :: GetLine(int handle) {
    return m_stc->MarkerLineFromHandle(handle) + 1;
}

int VerilogManager::GetSTCLine(int handle) {
    return m_stc->MarkerLineFromHandle(handle);
}


wxString VerilogManager::GetText(const Block& b) {
   return GetBlockText(b);
}

wxString VerilogManager::GetBlockText(const Block& b) {
    int startLine = GetSTCLine(b.startHandle);
    int endLine = GetSTCLine(b.endHandle);
    int lineCount = m_stc->GetLineCount();
    if (startLine < 0 || endLine < startLine || lineCount <= 0) {
        return wxEmptyString;
    }

    startLine = std::clamp(startLine, 0, lineCount - 1);
    endLine = std::clamp(endLine, startLine, lineCount - 1);
    int startPos = m_stc->PositionFromLine(startLine);
    int endPos = endLine + 1 < lineCount
        ? m_stc->PositionFromLine(endLine + 1)
        : m_stc->GetTextLength();
    return m_stc->GetTextRange(startPos, endPos);
}

void VerilogManager::Print() {
    for (auto b : blocks) {
        wxString info = wxString::Format("Block: %d - %d, Node: %s\n", GetLine(b.startHandle),
            GetLine(b.endHandle), wxString::FromUTF8(b.self->GetName().c_str()));
        OutputDebugStringA(info);
    }

    for (auto b : break_blocks) {
        wxString code = m_stc->GetTextRange(b.structure->Start(), b.structure->End());
        wxString info = wxString::Format("Break_Block: %d - %d\n\tCode: %s\n", GetLine(b.startHandle), GetLine(b.endHandle), code);
        OutputDebugStringA(info);
    }
}

void VerilogManager::SigFlowNodeAdded(SigTreeNode* node) {
    // Project parsing also emits node-added events.  Until SetFileNode() has
    // bound an actual source file, those events must not mutate the editor.
    if (!node || !m_stc || !fn) {
        return;
    }

    if (!recovering_block) {
        SigTreeNode* p = node->GetParent();
        if (!p) {
            return;
        }

        wxString text = node->ToVerilog();
        if (text.IsEmpty()) {
            return;
        }

        if (p->type == SigTreeNodeType::File) {
            // A file-level module may only be appended to the file currently
            // displayed by this manager.  Without this ownership check,
            // loading a project dirties the pathless editor with parsed nodes.
            if (p != fn || m_stc->GetCurrentPath().IsEmpty()) {
                return;
            }

            const wxString currentText = m_stc->GetText();
            const int startpos = m_stc->GetTextLength();
            if (!currentText.IsEmpty() && !currentText.EndsWith("\n") && !currentText.EndsWith("\r")) {
                text.Prepend("\n");
            }

            m_stc->m_isLoading = true;
            m_stc->InsertText(startpos, text);
            m_stc->m_isLoading = false;

            const int startline = m_stc->LineFromPosition(startpos);
            const int endline = m_stc->LineFromPosition(startpos + static_cast<int>(text.length()));
            AddBlock(startline, std::max(startline, endline), node);
            return;
        }

        auto it = std::find_if(blocks.begin(), blocks.end(), [p](const Block& b) {
            return b.self == p; // 这里写你的匹配逻辑
            });
        if (it != blocks.end()) {
            Block* parent  = &(*it);
            int endLine = GetLine(parent->endHandle)-1;
            int startpos = m_stc->PositionFromLine(endLine);
            m_stc->m_isLoading = true;
            m_stc->InsertText(startpos, text);
            m_stc->m_isLoading = false;
            int startline = m_stc->LineFromPosition(startpos);
            int endline = m_stc->LineFromPosition(startpos + static_cast<int>(text.length()));
            AddBlock(startline, std::max(startline, endline), node);
        }

    }
    else {
        auto it = std::find_if(break_blocks.begin(), break_blocks.end(), [this](const Block& item) {
            return &item == recovering_block;
            });

        if (it != break_blocks.end()) {
            if (recovering_block->structure) {
                recovering_block->structure->Clear();
            }
            recovering_block->structure = nullptr;
            it->self = node; // 将解析出的新语义节点赋值给 self

            // 3. 将其移入稳定集合 blocks
            blocks.splice(blocks.end(), break_blocks, it);
            //m_tree->AddChild(n->GetParent(), n); // 将新节点挂回树结构
        }
        recovering_block = nullptr;
        editing_top_block = nullptr;
    }

}

void VerilogManager::SigFlowNodeDeleted(SigTreeNode* node) {
}


std::vector<Stability> VerilogManager::GetLineStatus() {
    std::vector<Stability> sta = std::vector<Stability>(m_stc->GetLineCount(), Stability::Stable);
    if (StructureTest(m_stc->GetText().ToStdString())) {
        return sta;
    }

    for (Block& b : break_blocks) {
        int start_line = GetSTCLine(b.startHandle);
        int end_line = GetSTCLine(b.endHandle);
        for (int i = start_line; i <= end_line; i++) {
            if (i >= 0 && i < sta.size()) {
                sta[i] = Stability::Corrupted;
            }
        }
    }
    return sta;
}


void VerilogManager::UpdateFolding() {
    int maxLine = m_stc->GetLineCount();
    if (maxLine == 0) return;

    // 1. 初始化深度数组，默认全是基准层级 1024
    std::vector<int> depth(maxLine, 0);
    std::vector<bool> headerFlags(maxLine, false);

    for (const auto& b : blocks) {
        // 获取 STC 内部 0 进制行号
        if (b.self->type == SigTreeNodeType::File) continue; // 文件节点不参与折叠
        int s = m_stc->MarkerLineFromHandle(b.startHandle);
        int e = m_stc->MarkerLineFromHandle(b.endHandle);

        // 基础校验：Handle 必须有效且在当前范围内
        if (s < 0 || e < 0 || s >= maxLine || e >= maxLine) continue;

        // 只有跨行块才允许折叠
        if (e > s) {
            headerFlags[s] = true; // 起始行标记为“折叠头”

            // 从起始行的下一行到结束行，全部深度 +1
            for (int i = s + 1; i <= e; ++i) {
                depth[i]++;
            }
        }
    }

    // 2. 统一写入 STC
    for (int i = 0; i < maxLine; ++i) {
        int level = wxSTC_FOLDLEVELBASE + depth[i];
        if (headerFlags[i]) {
            level |= wxSTC_FOLDLEVELHEADERFLAG;
        }
        m_stc->SetFoldLevel(i, level);
    }
}

void VerilogManager::DrawBlockInfo() {
    // 1. 清除之前的所有注记
    m_stc->AnnotationClearAll();

    for (const auto& b : blocks) {
        int startLine = GetSTCLine(b.startHandle);
        if (startLine == -1) continue;

        // 2. 准备要显示的信息（例如从 SigTreeNode 中获取）
        wxString info = "";
        if (b.self) {
            info = wxString::Format("  [%s]", wxString::FromUTF8(b.self->GetName().c_str()));
        }
        else {
            info = "  [Analyzing...]";
        }

        // 3. 设置注记内容和样式
        m_stc->AnnotationSetText(startLine, info);
        m_stc->AnnotationSetStyle(startLine, 11); // 使用样式 ID 10
    }
}
