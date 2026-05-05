#include "VerilogManager.h"
#include "SigTree.h"
#include "SigTextEditor.h"
#include "VerilogStructuring.h"
#include <wx/time.h>

VerilogManager::VerilogManager(SigTextEditor* stc, SigFlowTree* tree, TSParser* parser) : m_stc(stc), m_tree(tree),m_ts_parser(parser) {
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

void VerilogManager::OnTimer(wxTimerEvent&) {
    for (Block& b : GetBreakBlocks()) {
        Structuring* s = b.structure;
        if (s->IsEmpty())
            return;

        int L = s->Start();
        int R = s->End();

        wxString delta = m_stc->GetTextRange(L, R);

        std::string x = "module _tmp;\n" + delta.ToStdString() + "\nendmodule\n";
        AnaPac ap = StructuringX(x);
        if (!ap.has_error) {
            // 1. 获取基础数据
            std::string fp = m_stc->m_currentFilePath.ToStdString();
            std::string fullCode = m_stc->GetText().ToStdString();

            ts_tree_delete(m_ts_tree);
            m_ts_tree = ts_parser_parse_string(m_ts_parser, nullptr, fullCode.c_str(), fullCode.length());

            TSTreeCursor cursor = ts_tree_cursor_new(ap.node);
            std::unordered_map<SigTreeNode*, std::tuple<int, int>> map;

            // 找到该文件对应的根节点开始同步
            SigTreeNode* fileNode = m_tree->GetFileNode(fp);
            if (fileNode) {
                if (b.self == nullptr) {
                    m_tree->UpdateTreeFromTS(&cursor, editing_top_block->self, fp, x, map);
                }
            }
            ts_tree_cursor_delete(&cursor);
            
            s->Clear();
        }
    }   
    m_stc->RenderLineMarker(GetLineStatus());
    UpdateFolding();
    m_stc->DebugFoldLevels();
    DrawBlockInfo();
    Print();
}

void VerilogManager::SetFileNode(FileNode* n, std::unordered_map<SigTreeNode*, std::tuple<int, int>> map){
    this->fn = n;
    ProjectNode* pn = static_cast<ProjectNode*>(n->GetParent());
    std::string path = n->filePath;
    m_stc->OpenFile(n->filePath);
    std::string text = m_stc->GetText().ToStdString();
    if (m_ts_tree) ts_tree_delete(m_ts_tree);
    m_ts_tree = ts_parser_parse_string(m_ts_parser, nullptr, text.c_str(), text.length());
    TSNode root = ts_tree_root_node(m_ts_tree);
    TSTreeCursor cursor = ts_tree_cursor_new(root);
    CollectBlocks(map);

}


void VerilogManager::CollectBlocks(std::unordered_map<SigTreeNode*, std::tuple<int, int>>& map) {
    // 1. 清理旧的标记，防止多次解析后句柄堆积
    m_stc->MarkerDeleteAll(BLOCK_MARKER_ID);
    blocks.clear();

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
    
    std::sort(blocks.begin(), blocks.end(), [this](const Block& a, const Block& b) {
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
    break_blocks.push_back({ s, e, nullptr });
    Structuring st;
    int pos = m_stc->PositionFromLine(startline);
    wxString text = m_stc->GetTextRange(pos, m_stc->PositionFromLine(endline));
    st.OnInsert(pos, text.ToStdString());
    structures.push_back(st);
    Block* b = &break_blocks.back();
    b->structure = &structures.back();
    return b;
}

void VerilogManager::RecoverBreakBlock(Block* b, SigTreeNode* n) {
    if (!b) return;
    auto it = std::find_if(break_blocks.begin(), break_blocks.end(), [b](const Block& item) {
        return &item == b;
        });

    if (it != break_blocks.end()) {
        b->structure->Clear();
        b->structure = nullptr;
        it->self = n; // 将解析出的新语义节点赋值给 self

        // 3. 将其移入稳定集合 blocks
        blocks.push_back(std::move(*it));
        //m_tree->AddChild(n->GetParent(), n); // 将新节点挂回树结构
        break_blocks.erase(it);
    }
    recovering_block = nullptr;
    editing_top_block = nullptr;
}


// ============== 异常块管控（HousekeepBlocks 系列） ==============
// 设计目标：在每次 EditBlock 进入归属判断之前，先把 blocks/break_blocks 中
// 因 Scintilla marker 行级特性导致的边界异常修复到一致状态。涵盖三类异常：
//   (1) 死亡：两端 handle 均失效 -> 整体销毁并从 SFTree 中删除节点
//   (2) 半死亡：一端 handle 失效  -> 销毁失效端，用存活端行号重建，块缩为单行
//   (3) 单行：startHandle 与 endHandle 同行 -> 销毁旧 endHandle，按事件语义重建
//        - 若本次为 INSERT 且包含 \n，且修改起始行命中该单行块所在行，则把
//          endHandle 迁移到 LineFromPosition(pos+len)（块扩展，吸纳新行）
//        - 否则在同一行 MarkerAdd 一个新的独立 handle（仅 ID 唯一化）
void VerilogManager::DestroyBlock(Block& b) {
    // 1) 解除可能指向本块的"游标指针"，防止后续逻辑访问到已销毁块
    if (recovering_block == &b)   recovering_block = nullptr;
    if (editing_top_block == &b)  editing_top_block = nullptr;

    // 2) 同步从 SFTree 删除节点（触发 EVT_SIGFLOWNODE_DEL，画布/属性面板/树同步）
    if (b.self) {
        SigTreeNode* parent = b.self->GetParent();
        if (parent) m_tree->RemoveChild(parent, b.self);
        b.self = nullptr;
    }

    // 3) 销毁 Scintilla marker handle（对已失效 handle 调用也是安全的）
    m_stc->MarkerDeleteHandle(b.startHandle);
    m_stc->MarkerDeleteHandle(b.endHandle);
    b.startHandle = -1;
    b.endHandle = -1;

    // 4) 释放 Structuring 缓冲（不从 std::list<Structuring> structures 中 erase，
    //    避免迭代器额外失效成本；Clear() 后该节点变为空缓冲不会影响其他块）
    if (b.structure) {
        b.structure->Clear();
        b.structure = nullptr;
    }
}

void VerilogManager::HousekeepVector(std::vector<Block>& vec,
                                     int eventType, int pos, int len, int newlineCount) {
    bool isInsert = (eventType & wxSTC_MOD_INSERTTEXT) != 0;
    int maxLine = m_stc->GetLineCount() - 1;
    if (maxLine < 0) maxLine = 0;

    // 反向遍历：erase 当前下标不会让更小下标的元素地址漂移
    for (int i = static_cast<int>(vec.size()) - 1; i >= 0; --i) {
        Block& b = vec[i];
        int s = m_stc->MarkerLineFromHandle(b.startHandle);
        int e = m_stc->MarkerLineFromHandle(b.endHandle);

        // ----- (1) 死亡：两端 handle 均失效 -----
        if (s == -1 && e == -1) {
            DestroyBlock(b);
            vec.erase(vec.begin() + i);
            continue;
        }

        // ----- (2) 半死亡：一端 handle 失效 -----
        if (s == -1) {
            m_stc->MarkerDeleteHandle(b.startHandle);
            b.startHandle = m_stc->MarkerAdd(e, BLOCK_MARKER_ID);
            s = e;
        }
        else if (e == -1) {
            m_stc->MarkerDeleteHandle(b.endHandle);
            b.endHandle = m_stc->MarkerAdd(s, BLOCK_MARKER_ID);
            e = s;
        }

        // ----- (3) 单行：销毁旧 endHandle 并按事件语义重建 -----
        if (s == e) {
            m_stc->MarkerDeleteHandle(b.endHandle);

            int newEndLine = s;
            if (isInsert && newlineCount > 0) {
                int evtStartLine = m_stc->LineFromPosition(pos);
                int evtEndLine = m_stc->LineFromPosition(pos + len);
                // 仅当本次插入起始行 == 该单行块所在行时，
                // 才把 endHandle 迁移到新行（块扩展语义）。
                if (evtStartLine == s) {
                    newEndLine = evtEndLine;
                }
            }
            if (newEndLine > maxLine) newEndLine = maxLine;
            if (newEndLine < 0)       newEndLine = 0;

            b.endHandle = m_stc->MarkerAdd(newEndLine, BLOCK_MARKER_ID);
        }
        // ----- (4) 多行：handles 自动跟踪，无需干预 -----
    }
}

void VerilogManager::HousekeepBlocks(int eventType, int pos, int len, const wxString& text) {
    // 计算插入/删除文本中的换行数量。仅 INSERT 事件才用于"行末换行扩展"判断；
    // DELETE 事件下 newlineCount 保持 0，不会触发扩展逻辑。
    int newlineCount = 0;
    if (eventType & wxSTC_MOD_INSERTTEXT) {
        for (size_t i = 0; i < text.size(); ++i) {
            if (text[i] == wxT('\n')) ++newlineCount;
        }
    }

    HousekeepVector(blocks,       eventType, pos, len, newlineCount);
    HousekeepVector(break_blocks, eventType, pos, len, newlineCount);
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

    // 先把所有块修复到一致状态：清理死亡块、修复半死亡块、重建/迁移单行块的 endHandle。
    // 必须在 FindBlock 之前执行，确保归属判断与后续 Structuring 累积都基于干净状态。
    HousekeepBlocks(type, pos, len, text);

    int start = m_stc->LineFromPosition(pos);
    int end = m_stc->LineFromPosition(pos + static_cast<int>(text.size()));
    Block* b = FindBlock(start, end);
    if (!b) {
        b = AddBreakBlock(start, end);
    }

    if (b->isStable()) {
        auto it = std::find_if(blocks.begin(), blocks.end(), [b](const Block& item) {
            return &item == b; // 通过地址匹配
            });

        if (it != blocks.end()) {
            SigTreeNode* n = it->self;
            it->self = nullptr;
            Structuring s;
            int startPos = m_stc->PositionFromLine(GetLine(b->startHandle)- 1);

            // 2. 获取结束行的行尾位置
            // 注意：第 N 行的行尾，实际上就是第 N+1 行的行首
            int endPos = m_stc->PositionFromLine(GetLine(b->endHandle));

            // 3. 此时获取的才是完整的跨行文本
            wxString t = m_stc->GetTextRange(startPos, endPos);
            s.OnInsert(startPos, t.ToStdString());
            structures.push_back(s);
            it->structure = &structures.back();
            break_blocks.push_back(std::move(*it));
            blocks.erase(it);
            m_tree->RemoveChild(n->GetParent(), n);
        }
    }

    Structuring* sp = b->structure;
    recovering_block = b;
    if (type & wxSTC_MOD_INSERTTEXT)
    {
        sp->OnInsert(pos, text.ToStdString());
    }

    if (type & wxSTC_MOD_DELETETEXT)
    {
        sp->OnDelete(pos, len);
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
   return m_stc->GetTextRange(m_stc->PositionFromLine(GetLine(b.startHandle)), m_stc->PositionFromLine(GetLine(b.endHandle)));
}

void VerilogManager::Print() {
    // 调试日志：输出每个 Block 的 handle ID + Scintilla 原始行号 + 状态标签，
    // 便于回归 "Block 3 起始单行 / Block 2 编辑缩成单行 / 整块删除" 等用例。
    auto stateTag = [](int s, int e) -> const char* {
        if (s == -1 && e == -1) return "[DEAD]";
        if (s == -1 || e == -1) return "[HALF]";
        if (s == e)             return "[SINGLE]";
        return "[MULTI]";
    };

    OutputDebugStringA("---- VerilogManager::Print ----\n");
    for (auto& b : blocks) {
        int s = m_stc->MarkerLineFromHandle(b.startHandle);
        int e = m_stc->MarkerLineFromHandle(b.endHandle);
        wxString info = wxString::Format(
            "Block %s sH=%d (line=%d) eH=%d (line=%d) Node=%s\n",
            stateTag(s, e),
            b.startHandle, s,
            b.endHandle,   e,
            b.self ? b.self->GetName() : "<null>");
        OutputDebugStringA(info);
    }

    for (auto& b : break_blocks) {
        int s = m_stc->MarkerLineFromHandle(b.startHandle);
        int e = m_stc->MarkerLineFromHandle(b.endHandle);
        wxString code = b.structure
            ? m_stc->GetTextRange(b.structure->Start(), b.structure->End())
            : wxString("<no-structure>");
        wxString info = wxString::Format(
            "BreakBlock %s sH=%d (line=%d) eH=%d (line=%d)\n\tCode: %s\n",
            stateTag(s, e),
            b.startHandle, s,
            b.endHandle,   e,
            code);
        OutputDebugStringA(info);
    }
    OutputDebugStringA(wxString::Format("blocks=%zu break_blocks=%zu structures=%zu\n",
                                        blocks.size(), break_blocks.size(), structures.size()));
}

void VerilogManager::SigFlowNodeAdded(SigTreeNode* node) {
    if (!recovering_block) {
        SigTreeNode* p = node->GetParent();
        auto it = std::find_if(blocks.begin(), blocks.end(), [p](const Block& b) {
            return b.self == p; // 这里写你的匹配逻辑
            });
        if (it != blocks.end()) {
            Block* parent  = &(*it);
            int endLine = GetLine(parent->endHandle)-1;
            int startpos = m_stc->PositionFromLine(endLine);
            m_stc->m_isLoading = true;
            wxString text = node->ToVerilog();
            m_stc->InsertText(startpos, text);
            m_stc->m_isLoading = false;
            int startline = m_stc->LineFromPosition(startpos);
            int endline = m_stc->LineFromPosition(startpos + static_cast<int>(text.size()))-2;
            AddBlock(startline, endline, node);
        }

    }
    else {
        auto it = std::find_if(break_blocks.begin(), break_blocks.end(), [this](const Block& item) {
            return &item == recovering_block;
            });

        if (it != break_blocks.end()) {
            recovering_block->structure->Clear();
            recovering_block->structure = nullptr;
            it->self = node; // 将解析出的新语义节点赋值给 self

            // 3. 将其移入稳定集合 blocks
            blocks.push_back(std::move(*it));
            //m_tree->AddChild(n->GetParent(), n); // 将新节点挂回树结构
            break_blocks.erase(it);
        }
        recovering_block = nullptr;
        editing_top_block = nullptr;
    }

}

void VerilogManager::SigFlowNodeDeleted(SigTreeNode* node) {

}


std::vector<Stability> VerilogManager::GetLineStatus() {
    std::vector<Stability> sta = std::vector<Stability>(m_stc->GetLineCount(), Stability::Stable);
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
            info = wxString::Format("  [%s]", b.self->GetName());
        }
        else {
            info = "  [Analyzing...]";
        }

        // 3. 设置注记内容和样式
        m_stc->AnnotationSetText(startLine, info);
        m_stc->AnnotationSetStyle(startLine, 11); // 使用样式 ID 10
    }
}
