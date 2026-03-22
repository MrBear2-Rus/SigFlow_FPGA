#include <wx/graphics.h> 
#include <wx/dcbuffer.h>
#include <wx/dcgraph.h>  
#include <wx/filename.h>
#include <wx/file.h>
#include <wx/stdpaths.h>
#include <fstream>



#include "CanvasPanel.h"
#include "my_log.h"
#include "json.hpp"

#include "ToolStateMachine.h"
#include "HandyToolKit.h"
#include "CanvasEventHandler.h"
#include "CanvasNoteBook.h"

wxBEGIN_EVENT_TABLE(CanvasPanel, wxPanel)
EVT_PAINT(CanvasPanel::OnPaint)
EVT_LEFT_DOWN(CanvasPanel::OnLeftDown)
EVT_LEFT_UP(CanvasPanel::OnLeftUp)
EVT_LEFT_DCLICK(CanvasPanel::OnLeftDoubleClick)
EVT_RIGHT_DOWN(CanvasPanel::OnRightDown)
EVT_RIGHT_UP(CanvasPanel::OnRightUp)
EVT_MOTION(CanvasPanel::OnMouseMove)
EVT_KEY_DOWN(CanvasPanel::OnKeyDown)
EVT_MOUSEWHEEL(CanvasPanel::OnMouseWheel)
EVT_SET_FOCUS(CanvasPanel::OnFocus)
EVT_KILL_FOCUS(CanvasPanel::OnKillFocus)
EVT_SCROLL(CanvasPanel::OnScroll)
EVT_COMMAND(wxID_ANY, EVT_SFTREE_NODE_ACTIVATED, CanvasPanel::OnSFNodeActivated) 
wxEND_EVENT_TABLE()
wxDEFINE_EVENT(wxEVT_CANVAS_MODIFIED, wxCommandEvent);


CanvasPanel::CanvasPanel(CanvasNoteBook* parent, SigFlowTree* sftree, size_t size_x, size_t size_y)
    : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize,
        wxFULL_REPAINT_ON_RESIZE | wxBORDER_NONE),
    m_mainFrame(parent),
    sftree(sftree),
    m_size{wxSize(size_x,size_y)},
    m_grid(20),
    m_hoverInfo{}, m_hasFocus(false),
    m_hiddenTextCtrl(nullptr),
    m_isUsingHiddenCtrl(false), m_currentEditingTextIndex(-1) ,
    m_isModified(false) {
    SetupHiddenTextCtrl();

    //滚动条
    m_vScroll = new wxScrollBar(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxSB_VERTICAL);
    m_hScroll = new wxScrollBar(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxSB_HORIZONTAL);
    LayoutScrollbars();

    // 工具状态机
    m_toolStateMachine = new ToolStateMachine(this);

    // 工具管理器
    m_CanvasEventHandler = new CanvasEventHandler(this, m_toolStateMachine);

    // 快捷工具栏
    m_HandyToolKit = new HandyToolKit(this, m_CanvasEventHandler);

    m_CanvasEventHandler->SetCurrentTool(ToolType::SELECT_TOOL);

    SetBackgroundStyle(wxBG_STYLE_PAINT);
    SetBackgroundColour(*wxWHITE);


    SetScale(1.0);
    SetoffSet(wxPoint(0, 0));
    SetFocus();
}

void CanvasPanel::OnLeftDown(wxMouseEvent& evt){
    EnsureFocus();
    m_HandyToolKit->Hide();

    m_CanvasEventHandler->OnCanvasLeftDown(evt);
}

void CanvasPanel::OnMouseMove(wxMouseEvent& evt) {
    UpdateHoverInfo(evt.GetPosition());

    m_CanvasEventHandler->OnCanvasMouseMove(evt);
}

void CanvasPanel::OnLeftUp(wxMouseEvent& evt){
    m_CanvasEventHandler->OnCanvasLeftUp(evt);

}

void CanvasPanel::OnLeftDoubleClick(wxMouseEvent& evt) {
    m_CanvasEventHandler->OnCanvasLeftDoubleClick(evt);
}

void CanvasPanel::OnKeyDown(wxKeyEvent& evt) {
    m_CanvasEventHandler->OnCanvasKeyDown(evt);

}

void CanvasPanel::OnMouseWheel(wxMouseEvent& evt) {
    m_CanvasEventHandler->OnCanvasMouseWheel(evt);

}

void CanvasPanel::SetScale(float scale) {
    float min_scale, max_scale;
    std::tie(min_scale, max_scale) = ValidScaleRange();
    if (scale < min_scale) scale = min_scale;
    if (scale > max_scale) scale = max_scale;
    m_scale = scale;
    Refresh(); 
}

wxPoint CanvasPanel::ClientToCanvas(const wxPoint& screenPos) const{
    return wxPoint(
        static_cast<int>((screenPos.x - m_offset.x) / m_scale),
        static_cast<int>((screenPos.y - m_offset.y) / m_scale)
    );
}

wxPoint CanvasPanel::CanvasToClient(const wxPoint& canvasPos) const
{
    return wxPoint(
        static_cast<int>(canvasPos.x * m_scale + m_offset.x),
        static_cast<int>(canvasPos.y * m_scale + m_offset.y)
    );
}

void CanvasPanel::OnPaint(wxPaintEvent&) {
    LayoutScrollbars();
    wxAutoBufferedPaintDC dc(this);
    dc.Clear();

    wxRect updateRect = GetUpdateRegion().GetBox();

    std::unique_ptr<wxGraphicsContext> gc(wxGraphicsRenderer::GetDefaultRenderer()->CreateContext(dc));


    if (gc) {

        // 应用缩放和偏移（逻辑坐标 -> 设备坐标）
        gc->Scale(m_scale, m_scale);
        gc->Translate(m_offset.x / m_scale, m_offset.y / m_scale);

        // 绘制网格（逻辑坐标，线宽随缩放自适应）
        const wxColour gridColor(240, 240, 240);
        //const wxColour gridColor(50, 60, 80);
        gc->SetPen(wxPen(gridColor, 1.0 / m_scale)); // 笔宽在逻辑坐标下调整

        wxSize sz = wxSize(m_size.x + 1, m_size.y + 1);
        int maxX = static_cast<int>(sz.x);
        int maxY = static_cast<int>(sz.y);

        for (int x = 0; x < maxX; x += m_grid) {
            gc->StrokeLine(x, 0, x, maxY);
        }
        for (int y = 0; y < maxY; y += m_grid) {
            gc->StrokeLine(0, y, maxX, y);
        }

        // 绘制导线（矢量线段）
        gc->SetPen(wxPen(*wxBLACK, 1.5 / m_scale)); // 导线宽度自适应
        for (const auto& w : m_wires) w.Draw(gc.get());
        if (m_toolStateMachine->GetWireState() == WireToolState::WIRE_DRAWING) m_previewWire.Draw(gc.get());

        // 绘制预览元素
        if (m_toolStateMachine->GetComponentState() == ComponentToolState::COMPONENT_PREVIEW) {
            m_previewElement.Draw(gc.get());
        }

        if(!m_tbox.m_shapes.empty()) m_tbox.Draw(gc.get());

        // 绘制元素（使用矢量绘制）
        for (size_t i = 0; i < m_elems.size(); ++i) {
            m_elems[i].Draw(gc.get()); // 确保元素内部使用gc绘制
        }

        /* 元件形状调试
        CanvasElement ele = CloneElement("XNOR_Gate").value();
        wxPoint pos = wxPoint(100, 100);
        ele.SetPos(pos);
        ele.Draw(gc.get());
        wxRect r = ele.GetBounds();

        double radius = 3.0; // 点的半径

        gc->SetBrush(*wxRED_BRUSH); // 填充颜色
        gc->SetPen(*wxTRANSPARENT_PEN); // 无边框
        // 绘制一个以 pt 为中心的圆
        gc->DrawEllipse(pos.x - radius, pos.y - radius, radius * 2, radius * 2);

        gc->SetBrush(wxBrush(wxColour(200, 200, 200, 128))); // 半透明灰色
        gc->SetPen(wxPen(*wxBLACK, 2)); // 2像素宽的黑边
        gc->DrawRectangle(r.x, r.y, r.width, r.height);*/

        // 悬停引脚高亮（绿色空心圆）
        if (m_hoverInfo.IsOverPin()) {
            gc->SetBrush(*wxTRANSPARENT_BRUSH);
            gc->SetPen(wxPen(wxColour(0, 255, 0), 1.0 / m_scale));
            gc->DrawEllipse(m_hoverInfo.snappedPos.x - 3, m_hoverInfo.snappedPos.y - 3, 6, 6);
        }

        if (m_hoverInfo.IsOverCell()) {
            gc->SetBrush(*wxTRANSPARENT_BRUSH);
            gc->SetPen(wxPen(wxColour(0, 255, 0), 1.0 / m_scale));
            gc->DrawEllipse(m_hoverInfo.cellPos.x - 3, m_hoverInfo.cellPos.y - 3, 6, 6);

            gc->SetPen(wxPen(wxColour(*wxBLACK), 1.0 / m_scale));
            gc->SetBrush(wxColour(*wxBLACK));
            const std::vector<Wire>& ws = GetWires();
            if (ws.size() > m_hoverInfo.wireIndex) {
                Wire w = ws[m_hoverInfo.wireIndex];
                wxPoint sectionStart = w.pts[m_hoverInfo.wireSectionIndex].pos;
                wxPoint sectionEnd = w.pts[m_hoverInfo.wireSectionIndex + 1].pos;
                wxPoint sectionMid = (sectionStart + sectionEnd) / 2;
                gc->DrawRectangle(sectionMid.x - 3, sectionMid.y - 3, 6, 6);
            }

        }

        

        //if (m_hoverInfo.IsOverMidCell()) {
        //    gc->SetPen(wxPen(wxColour(*wxBLACK), 1.0 / m_scale));
        //    gc->SetBrush(wxColour(*wxBLACK));
        //    gc->DrawEllipse(GetWires()[m_hoverInfo.midWireIndex].midCells[m_hoverInfo.midCellIndex].pos.x-6, GetWires()[m_hoverInfo.midWireIndex].midCells[m_hoverInfo.midCellIndex].pos.y - 6, 12, 12);
        //}

        // 绘制文本元素 - 修改为使用 unique_ptr
        for (auto& textElem : m_textElements) {
            textElem.Draw(gc.get());
        }

        // 绘制选中边框
        if (m_toolStateMachine->GetCurrentTool() == ToolType::SELECT_TOOL) {
            for (size_t i = 0; i < m_selElemIdx.size(); i++) {
                wxRect b = m_elems[m_selElemIdx[i]].GetBounds();
                gc->SetPen(wxPen(wxColor(44, 145, 224), 2.0));
                gc->SetBrush(*wxTRANSPARENT_BRUSH);
                gc->DrawRectangle(b.x - 2, b.y - 3, b.width + 5, b.height + 5);

                gc->SetPen(wxPen(wxColor(44, 145, 224, 32), 2.0));
                gc->SetBrush(*wxTRANSPARENT_BRUSH);
                gc->DrawRectangle(b.x - 4, b.y - 5, b.width + 9, b.height + 9);

               gc->SetPen(wxPen(wxColor(44, 145, 224, 32), 4.0));
                gc->SetBrush(*wxTRANSPARENT_BRUSH);
                gc->DrawRectangle(b.x - 6, b.y - 7, b.width + 13, b.height + 13);
            }
            for (size_t i = 0; i < m_selTxtIdx.size(); i++) {
                wxRect b = m_textElements[m_selTxtIdx[i]].GetBounds();
                gc->SetPen(wxPen(wxColor(44, 145, 224), 2.0));
                gc->SetBrush(*wxTRANSPARENT_BRUSH);
                gc->DrawRectangle(b.x - 3, b.y - 3, b.width + 5, b.height + 5);

                gc->SetPen(wxPen(wxColor(44, 145, 224, 32), 2.0));
                gc->SetBrush(*wxTRANSPARENT_BRUSH);
                gc->DrawRectangle(b.x - 5, b.y - 5, b.width + 9, b.height + 9);

                gc->SetPen(wxPen(wxColor(44, 145, 224, 32), 4.0));
                gc->SetBrush(*wxTRANSPARENT_BRUSH);
                gc->DrawRectangle(b.x - 7, b.y - 7, b.width + 13, b.height + 13);
            }
            for (size_t i = 0; i < m_selWireIdx.size(); i++) {
                m_wires[m_selWireIdx[i]].DrawColor(gc.get());

            }
        }

        // 绘制选择边框
        if (m_toolStateMachine->GetSelectState() == SelectToolState::RECTANGLE_SELECT) {
            wxRect selRect = m_selectRect;
            gc->SetPen(wxPen(wxColor(44, 145, 224), 2 / m_scale));
            gc->SetBrush(wxColor(44, 145, 224, 32));
            gc->DrawRectangle(selRect.x, selRect.y, selRect.width, selRect.height);
        }

        if (m_toolStateMachine->GetEraserState() == EraserToolState::RECTANGLE_ERASER) {
            wxRect eraRect = m_eraserRect;
            gc->SetPen(wxPen(wxColor(128, 128, 128), 2 / m_scale));
            gc->SetBrush(wxColor(128, 128, 128, 32));
            gc->DrawRectangle(eraRect.x, eraRect.y, eraRect.width, eraRect.height);
        }

    }
}
std::tuple<bool, int> CanvasPanel::HitTopAndPinTest(const wxPoint& canvasPos, bool* isInput, wxPoint* worldPos) {
    bool over_top = false;
    int pin_id = -1;
    const auto& elem = m_tbox;

    // 输入引脚尖端（突出 1 px）
    for (size_t p = 0; p < elem.GetInputPins().size(); ++p) {
        wxPoint tip = elem.GetPos() + wxPoint(elem.GetInputPins()[p].pos.x - 1,
            elem.GetInputPins()[p].pos.y);
        if (abs(canvasPos.x - tip.x) <= 4 && abs(canvasPos.y - tip.y) <= 4) {
            *isInput = true;
            *worldPos = tip;
            over_top = true;
            pin_id = p;
        }
    }
    // 输出引脚尖端（突出 1 px）
    for (size_t p = 0; p < elem.GetOutputPins().size(); ++p) {
        wxPoint tip = elem.GetPos() + wxPoint(elem.GetOutputPins()[p].pos.x + 1,
            elem.GetOutputPins()[p].pos.y);
        if (abs(canvasPos.x - tip.x) <= 4 && abs(canvasPos.y - tip.y) <= 4) {
            *isInput = false;
            *worldPos = tip;
            over_top = true;
            pin_id = p;
        }
    }

    return std::tie(over_top, pin_id);
}

std::tuple<int, int> CanvasPanel::HitElementAndPinTest(const wxPoint& canvasPos, bool* isInput, wxPoint* worldPos)
{
    int elem_id = -1;
    int pin_id = -1;
    for (size_t i = 0; i < m_elems.size(); ++i) {
        // 元素的边界是画布坐标，直接比较
        if (m_elems[i].GetBounds().Contains(canvasPos)) {
            elem_id = i;
        }
        const auto& elem = m_elems[i];
        // 输入引脚尖端（突出 1 px）
        for (size_t p = 0; p < elem.GetInputPins().size(); ++p) {
            wxPoint tip = elem.GetPos() + wxPoint(elem.GetInputPins()[p].pos.x - 1,
                elem.GetInputPins()[p].pos.y);
            if (abs(canvasPos.x - tip.x) <= 4 && abs(canvasPos.y - tip.y) <= 4) {
                *isInput = true;
                *worldPos = tip;
                elem_id = i;
                pin_id = p;
            }
        }
        // 输出引脚尖端（突出 1 px）
        for (size_t p = 0; p < elem.GetOutputPins().size(); ++p) {
            wxPoint tip = elem.GetPos() + wxPoint(elem.GetOutputPins()[p].pos.x + 1,
                elem.GetOutputPins()[p].pos.y);
            if (abs(canvasPos.x - tip.x) <= 4 && abs(canvasPos.y - tip.y) <= 4) {
                *isInput = false;
                *worldPos = tip;
                elem_id = i;
                pin_id = p;
            }
        }

        // 输出引脚尖端（突出 1 px）
        for (size_t p = 0; p < elem.GetInOutputPins().size(); ++p) {
            wxPoint tip = elem.GetPos() + wxPoint(elem.GetInOutputPins()[p].pos.x + 1,
                elem.GetInOutputPins()[p].pos.y);
            if (abs(canvasPos.x - tip.x) <= 4 && abs(canvasPos.y - tip.y) <= 4) {
                *isInput = false;
                *worldPos = tip;
                elem_id = i;
                pin_id = p;
            }
        }
    }
    return std::tie(elem_id, pin_id);
}

int CanvasPanel::HitWire(const wxPoint& canvasPos) {
    // 首先将点击位置对齐到最近的网格点
    wxPoint snappedPos = Snap(canvasPos);
    for (size_t w = 0; w < m_wires.size(); ++w) {
        const auto& wire = m_wires[w];
        for (size_t c = 0; c < wire.cells.size(); ++c) {
            const wxPoint& cell = wire.cells[c].pos;

            // 检查是否在网格点上且与对齐后的点击位置匹配
            if (cell.x == snappedPos.x && cell.y == snappedPos.y) {
                // 二次验证：确保在点击半径内
                if (abs(canvasPos.x - cell.x) <= HIT_RADIUS &&
                    abs(canvasPos.y - cell.y) <= HIT_RADIUS) {
                    return w;
                }
            }
        }
    }
    return -1;
}

int CanvasPanel::HitHoverCell(const wxPoint& canvasPos) {
    int wireIdx = HitWire(canvasPos);
    if (wireIdx == -1) return -1;

    wxPoint snappedPos = Snap(canvasPos);

    // 用于暂存命中的非Mid节点（作为备选）
    int fallbackIndex = -1;

    for (size_t c = 0; c < m_wires[wireIdx].cells.size(); ++c) {
        const auto& cellData = m_wires[wireIdx].cells[c];
        const wxPoint& cell = cellData.pos;
        bool isHit = false;

        // --- 命中检测逻辑 (保持原样) ---
        if (cell.x == snappedPos.x && cell.y == snappedPos.y) {
            if (abs(canvasPos.x - cell.x) <= HIT_RADIUS &&
                abs(canvasPos.y - cell.y) <= HIT_RADIUS) {
                isHit = true;
            }
        }
        else {
            // 原逻辑：只有Mid类型才允许在没Snap住的情况下进行半径检测
            if (cellData.type == CellType::Mid) {
                if (abs(canvasPos.x - cell.x) <= HIT_RADIUS &&
                    abs(canvasPos.y - cell.y) <= HIT_RADIUS) {
                    isHit = true;
                }
            }
        }

        // --- 优先级处理逻辑 ---
        if (isHit) {
            if (cellData.type == CellType::Mid) {
                return c; // 优先级最高：一旦发现 Mid 命中，立即返回！
            }

            // 如果不是 Mid，但命中了，先记下来。
            // 只有当 fallbackIndex 还没被赋值时才赋值(保留第一个命中的)，
            // 或者你可以根据距离更新最近的一个。
            if (fallbackIndex == -1) {
                fallbackIndex = c;
            }
            // 关键：不要在这里 return，继续找后面有没有 Mid
        }
    }

    // 如果循环跑完都没找到 Mid，但找到了其他点，就返回那个点
    return fallbackIndex;
}

int CanvasPanel::HitWireSection(const wxPoint& canvasPos) {
    return GetWires()[HitWire(canvasPos)].cells[HitHoverCell(canvasPos)].pre_pts_idx;
}

void CanvasPanel::DeleteSelected() {


    Refresh();
    //触发改变
    SetModified(1);
}

using json = nlohmann::json;

void CanvasPanel::Save() {
    auto* n = tn->GetParent()->GetParent();
    ProjectNode* pn = static_cast<ProjectNode*>(n);
    wxString cwd = pn->projectPath;
    wxFileName targetDir;
    targetDir.AssignDir(cwd + "/.sigflow/canvas");

    if (!targetDir.DirExists()) {
        targetDir.Mkdir(wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL);
    }

    // 1. 修正文件名：加上 .json 后缀
    wxString filename = GetNote();
    if (!filename.EndsWith(".json")) {
        filename += ".json";
    }

    wxFileName filepath(targetDir.GetPath(), filename);

    // 2. 构建 JSON 数据结构
    json root;
    root["canvas_name"] = GetNote().ToStdString();
    root["elements"] = json::array(); // 初始化元素数组
    

    // 3. 遍历 m_elems 并填充数据
    for (const auto& elem : m_elems) {
        json element;
        element["type"] = elem.type.ToStdString();
        element["id"] = elem.GetIdentifier().ToStdString();
        // 假设 GetPos() 返回 wxPoint
        wxPoint pos = elem.GetPos();

        element["x"] = pos.x;
        element["y"] = pos.y;

        // 如果需要保存更多信息（例如大小、标识符），在这里继续添加
        // element["width"] = elem.m_bound.GetWidth();
        // element["identifier"] = elem.identifier.ToStdString();

        root["elements"].push_back(element);
    }
    json start, end;
    start["x"] = m_tbox.GetPos().x;
    start["y"] = m_tbox.GetPos().y;
    end["x"] = m_tbox.GetPos().x + m_tbox.GetBounds().width;
    end["y"] = m_tbox.GetPos().y + m_tbox.GetBounds().height;

    root["topbox"]["start"] = start;
    root["topbox"]["end"] = end;

    // 4. 将 JSON 写入文件
    std::ofstream file(filepath.GetFullPath().ToStdString());
    if (file.is_open()) {
        // 设置缩进为 4 个空格，使 JSON 可读
        file << root.dump(4);
        file.close();
    }
}



bool CanvasPanel::Read() {
    auto* n = tn->GetParent()->GetParent();
    ProjectNode* pn = static_cast<ProjectNode*>(n);
    wxString cwd = pn->projectPath;
    wxString name = GetNote();
    wxFileName filepath(cwd + "/.sigflow/canvas", name + ".json");

    if (!filepath.FileExists()) {
        return false;
    }

    std::ifstream file(filepath.GetFullPath().ToStdString());
    if (!file.is_open()) return false;

    json root;
    try {
        file >> root;
    }
    catch (...) {
        return false;
    }
    file.close();

    bool allElementsMatched = true;

    if (root.contains("elements") && root["elements"].is_array()) {
        // 1. 优化策略：创建一个 Identifier -> SecondElement* 的映射表
        // 这样查找时间复杂度是 O(N)，而不是 O(N^2)
        std::unordered_map<std::string, SecondElement*> idMap;
        for (auto& elem : m_elems) {
            idMap[elem.GetIdentifier().ToStdString()] = &elem;
        }

        // 2. 遍历 JSON 数据进行匹配更新
        for (const auto& jElem : root["elements"]) {
            if (jElem.contains("id") && jElem.contains("x") && jElem.contains("y")) {
                std::string id = jElem["id"].get<std::string>();

                // 3. 在映射表中查找对应元件
                if (idMap.find(id) != idMap.end()) {
                    int x = jElem["x"].get<int>();
                    int y = jElem["y"].get<int>();

                    // 4. 更新位置
                    idMap[id]->SetPos(wxPoint(x, y));
                }
                else allElementsMatched = false;
            }
        }
    }

    if (root.contains("topbox")) {
        wxPoint start(root["topbox"]["start"]["x"].get<int>(),
            root["topbox"]["start"]["y"].get<int>());

        wxPoint end(root["topbox"]["end"]["x"].get<int>(),
            root["topbox"]["end"]["y"].get<int>());
        m_tbox = TopModuleBox(start, end, tn);
    }

    Refresh(); // 重新绘制
    return allElementsMatched;
}




void CanvasPanel::OnRightDown(wxMouseEvent& evt) {
    m_HandyToolKit->SetPosition(ClientToScreen(evt.GetPosition()) + FromDIP(wxPoint(24, 0)));
    m_HandyToolKit->Show();
    m_HandyToolKit->SetFocus();
}

void CanvasPanel::OnRightUp(wxMouseEvent& evt) {
    m_HandyToolKit->Hide();
}

void CanvasPanel::SetStatus(wxString status) {
    m_mainFrame->SetStatusText(status, 0);

}

void CanvasPanel::SetCurrentTool(ToolType tool) {
    m_toolStateMachine->SetCurrentTool(tool);
}

void CanvasPanel::SetCurrentComponent(const wxString& componentName) {
    m_toolStateMachine->SetCurrentTool(ToolType::COMPONENT_TOOL);
    m_toolStateMachine->SetComponentState(ComponentToolState::COMPONENT_PREVIEW);
    m_CanvasEventHandler->SetCurrentComponent(componentName);
    SetPreview(componentName);
}

void CanvasPanel::UpdateHoverInfo(const wxPoint& screenPos) {
    m_hoverInfo.screenPos = screenPos;
    m_hoverInfo.canvasPos = ClientToCanvas(screenPos);
    m_hoverInfo.snappedPos = Snap(m_hoverInfo.canvasPos);

    // 悬停引脚和元件信息检测
    bool isInput = false;
    wxPoint pinWorldPos;
    int pinIdx = -1;
    int elementIndex = -1;
    std::tie(elementIndex, pinIdx)= HitElementAndPinTest(m_hoverInfo.canvasPos, &isInput, &pinWorldPos);

    bool overTop = false;
    bool istopInput = false;
    wxPoint pintopWorldPos;
    int pintopIdx = -1;
    std::tie(overTop, pintopIdx) = HitTopAndPinTest(m_hoverInfo.canvasPos, &istopInput, &pintopWorldPos);
    if (pintopIdx != -1) { pinIdx = pintopIdx; isInput = istopInput; pinWorldPos = pintopWorldPos; };

    // 导线控制点信息检测
    int wireIdx = HitWire(m_hoverInfo.canvasPos);
    int wireSectionIdx = -1;
    int cellIdx = HitHoverCell(m_hoverInfo.canvasPos);
    bool isCellMid = false;
    wxPoint cellWorldPos;

    if (wireIdx != -1 && cellIdx != -1) {
        Cell cell = m_wires[wireIdx].cells[cellIdx];
        isCellMid = cell.type == CellType::Mid ? true : false;
        cellWorldPos = cell.pos;
        wireSectionIdx = cell.pre_pts_idx;
    }


    // 悬停文本检测
    int textIndex = HitTestText(m_hoverInfo.canvasPos);

    // 更新信息
    m_hoverInfo.pinIndex = pinIdx;
    m_hoverInfo.isInputPin = isInput;
    m_hoverInfo.pinPos = pinWorldPos;

    m_hoverInfo.wireIndex = wireIdx;
    m_hoverInfo.wireSectionIndex = wireSectionIdx;
    m_hoverInfo.cellIndex = cellIdx;
    m_hoverInfo.isCellMiddle = isCellMid;
    m_hoverInfo.cellPos = cellWorldPos;

    m_hoverInfo.elementIndex = elementIndex;
    if (elementIndex != -1) m_hoverInfo.elementName = m_elems[elementIndex].GetIdentifier();
    else m_hoverInfo.elementName = "";

    m_hoverInfo.textIndex = textIndex;
    // Refresh(); // 触发重绘以显示悬停效果
    wxString hover = "";
    if (m_hoverInfo.IsOverPin()) {
        hover = (wxString::Format("%sPin[%d]",
            m_hoverInfo.isInputPin ? "Input" : "Output", m_hoverInfo.pinIndex));
    }
    else if (m_hoverInfo.IsOverCell()) {
        if (m_hoverInfo.isCellMiddle) {
            hover = (wxString::Format("Wire[%d] Section[%d] ControlPoint",
                m_hoverInfo.wireIndex, m_hoverInfo.wireSectionIndex));
        }
        else {
            hover = (wxString::Format("Wire[%d] Cell[%d]",
                m_hoverInfo.wireIndex, m_hoverInfo.cellIndex));
        }
    }
    else if (m_hoverInfo.IsOverElement()) {
        hover = (wxString::Format("Component[%s]",
            m_hoverInfo.elementName));
    }
    else if (m_hoverInfo.IsOverText()) {
        hover = (wxString::Format("TextBox[%d]",
            m_hoverInfo.textIndex));
    }
    else if (m_hoverInfo.IsOverTopBox()) {
        hover = (wxString::Format("TopBox[%s]",
            m_tbox.GetIdentifier()));
    }
    else {
        hover = "Blank Space";
    }

    wxString cursor = wxString::Format("(%d, %d)", m_hoverInfo.canvasPos.x, m_hoverInfo.canvasPos.y);

    // 修复后：
    wxString zoom = wxString::Format("%d%%", int(m_scale * 100));

    if (m_mainFrame) { // 检查是否正在退出
        m_mainFrame->SetStatusText(hover, 1);
        m_mainFrame->SetStatusText(cursor, 2);
        m_mainFrame->SetStatusText(zoom, 3);
    }

    m_CanvasEventHandler->UpdateHoverInfo(m_hoverInfo);
}

void CanvasPanel::OnFocus(wxFocusEvent& event) {
    m_hasFocus = true;
    Refresh();
    event.Skip();
}

void CanvasPanel::OnKillFocus(wxFocusEvent& event) {
    m_hasFocus = false;
    Refresh();
    event.Skip();
}

void CanvasPanel::EnsureFocus() {
    if (!m_hasFocus) {
        SetFocus();
    }
}

void CanvasPanel::SetupHiddenTextCtrl() {
    if (!m_hiddenTextCtrl) {
        m_hiddenTextCtrl = new wxTextCtrl(this, wxID_ANY, "",
            wxPoint(-1000, -1000), wxSize(1, 1),
            wxTE_PROCESS_ENTER | wxTE_RICH2);
        m_hiddenTextCtrl->Hide();

        m_hiddenTextCtrl->Bind(wxEVT_TEXT_ENTER, &CanvasPanel::OnHiddenTextCtrlEnter, this);
        m_hiddenTextCtrl->Bind(wxEVT_KILL_FOCUS, &CanvasPanel::OnHiddenTextCtrlKillFocus, this);
    }
}

void CanvasPanel::AttachHiddenTextCtrlToElement(int textIndex) {
    if (textIndex >= 0 && textIndex < (int)m_textElements.size()) {
        // 分离当前编辑的元素
        if (m_currentEditingTextIndex != -1 && m_currentEditingTextIndex < (int)m_textElements.size()) {
            m_textElements[m_currentEditingTextIndex].DetachHiddenTextCtrl();
            m_textElements[m_currentEditingTextIndex].StopEditing();
        }

        // 附加到新元素
        m_currentEditingTextIndex = textIndex;
        m_textElements[textIndex].AttachHiddenTextCtrl(m_hiddenTextCtrl);
        m_textElements[textIndex].StartEditing();

        // 显示隐藏TextCtrl（在正确位置）
        m_hiddenTextCtrl->Show();
        Refresh();
    }
}

void CanvasPanel::DetachHiddenTextCtrl() {
    if (m_currentEditingTextIndex != -1 && m_currentEditingTextIndex < (int)m_textElements.size()) {
        m_textElements[m_currentEditingTextIndex].DetachHiddenTextCtrl();
        m_textElements[m_currentEditingTextIndex].StopEditing();
        m_currentEditingTextIndex = -1;
    }

    m_hiddenTextCtrl->Hide();
    m_hiddenTextCtrl->SetPosition(wxPoint(-1000, -1000));
    Refresh();
}

void CanvasPanel::CreateTextElement(const wxPoint& position, wxString text) {
    m_textElements.push_back(std::move(CanvasTextElement(this, text, position)));
    AttachHiddenTextCtrlToElement(static_cast<int>(m_textElements.size() - 1));

    Refresh();
    //触发改变
    SetModified(1);
}

void CanvasPanel::AddTextWithIns(CanvasTextElement text) {
    m_textElements.push_back(std::move(text));
    Refresh();
}

void CanvasPanel::ReclaimText(CanvasTextElement text, int index) {
    m_textElements.insert(m_textElements.begin() + index, std::move(text));
    Refresh();
}

void CanvasPanel::CreateTextElementWithoutRecord(const wxPoint& position, wxString text) {
    m_textElements.push_back(std::move(CanvasTextElement(this, text, position)));
    //AttachHiddenTextCtrlToElement(static_cast<int>(m_textElements.size() - 1));

    Refresh();
}

void CanvasPanel::StartTextEditing(int index) {
    AttachHiddenTextCtrlToElement(index);
}

int CanvasPanel::HitTestText(wxPoint canvasPos) {
    for (int i = 0; i < m_textElements.size(); i++) {
        if (m_textElements[i].Contains(canvasPos)) return i;
    }
    return -1;
}

void CanvasPanel::OnScroll(wxScrollEvent& event) {
    if (event.GetOrientation() == wxHORIZONTAL) {
        m_offset.x = -event.GetPosition() * m_scale;
    }
    else {
        m_offset.y = -event.GetPosition() * m_scale;
    }
    Refresh();
}

void CanvasPanel::LayoutScrollbars(){
    wxSize clientSize = GetClientSize();
    int scrollbarSize = wxSystemSettings::GetMetric(wxSYS_VSCROLL_X);
    int width = 24;

    // 水平滚动条：底部，宽度要减去垂直滚动条的宽度
    m_hScroll->SetSize(clientSize.x - width, width);
    m_hScroll->SetPosition(wxPoint(0, clientSize.y - width));
    m_hScroll->SetThumbSize((clientSize.x - width - 1) / m_scale);
    m_hScroll->SetRange(m_size.x);
    m_hScroll->SetThumbPosition(-m_offset.x / m_scale);

    // 垂直滚动条：右侧，高度要减去水平滚动条的高度   
    m_vScroll->SetSize(width, clientSize.y);
    m_vScroll->SetPosition(wxPoint(clientSize.x - width, 0));
    m_vScroll->SetThumbSize((clientSize.y - width - 1) / m_scale);
    m_vScroll->SetRange(m_size.y);
    m_vScroll->SetThumbPosition(-m_offset.y / m_scale);

    Refresh();
}

void CanvasPanel::SetoffSet(wxPoint offset) {
    wxPoint min_offset, max_offset;
    std::tie(min_offset, max_offset) = ValidSetOffRange();
    if (offset.x < min_offset.x) offset.x = min_offset.x;
    if (offset.x > max_offset.x) offset.x = max_offset.x;
    if (offset.y < min_offset.y) offset.y = min_offset.y;
    if (offset.y > max_offset.y) offset.y = max_offset.y;
    m_offset = offset;
    Refresh();
}

wxPoint CanvasPanel::LogicToDevice(const wxPoint& logicPoint) const
{
    return wxPoint(
        static_cast<int>(logicPoint.x * m_scale + m_offset.x),
        static_cast<int>(logicPoint.y * m_scale + m_offset.y)
    );
}

wxPoint CanvasPanel::DeviceToLogic(const wxPoint& devicePoint) const
{
    return wxPoint(
        static_cast<int>((devicePoint.x - m_offset.x) / m_scale),
        static_cast<int>((devicePoint.y - m_offset.y) / m_scale)
    );
}

std::pair<wxPoint, wxPoint> CanvasPanel::ValidSetOffRange() {
    // (0, 0)的逻辑坐标对应的设备坐标为 m_offset，因此m_offset的最大值为(0,0)
    wxPoint maxOffset(0, 0);
    // 计算逻辑坐标 (m_size.x, m_size.y) 对应的设备坐标的最大值为 (GetClientSize().x- m_hScroll->GetSize().y - 1, GetClientSize().y- m_hScroll->GetSize().y - 1)，此时的 m_offset 即为最小值
    wxPoint minOffset(
        GetClientSize().x - static_cast<int>(m_size.x * m_scale) - m_hScroll->GetSize().y - 1,
        GetClientSize().y - static_cast<int>(m_size.y * m_scale) - m_hScroll->GetSize().y - 1
    );

    return std::make_pair(minOffset, maxOffset);
}

std::pair<float, float> CanvasPanel::ValidScaleRange() {
    // 对于最右侧最下侧的逻辑坐标 (m_size.x, m_size.y) 对应的设备坐标恰好为 (GetClientSize().x- m_hScroll->GetSize().y - 1, GetClientSize().y- m_hScroll->GetSize().y - 1) 时，计算出最小缩放比例
    wxSize s = GetClientSize();
    float h = m_hScroll->GetSize().y;
    float x = (s.x - h - 1) - m_offset.x;
    float y = (s.y - h - 1) - m_offset.y;
    float minScaleX = static_cast<float>(x) / static_cast<float>(m_size.x);
    float minScaleY = static_cast<float>(y) / static_cast<float>(m_size.y);
    float minScale = std::max(minScaleX, minScaleY);
    float maxScale = 5.0f; // 最大缩放比例
    return std::make_pair(minScale, maxScale);
}

void CanvasPanel::SetPreviewElement(const wxString& name, wxPoint pos) {
    /*
    extern std::vector<CanvasElement> g_elements;
    auto it = std::find_if(g_elements.begin(), g_elements.end(),
        [&](const CanvasElement& e) { return e.GetName() == name; });
    if (it == g_elements.end()) return;
    CanvasElement clone = *it;
    wxPoint standardpos = pos;
    const auto& outputPins = clone.GetOutputPins();
    const auto& inputPins = clone.GetInputPins();

    if (!outputPins.empty()) {
        // 优先使用输出引脚
        Pin standardPin = outputPins[0];
        standardpos = pos - wxPoint(standardPin.pos.x + m_grid, standardPin.pos.y - m_grid);
    }
    else if (!inputPins.empty()) {
        // 如果没有输出引脚，使用输入引脚
        Pin standardPin = inputPins[0];
        standardpos = pos - wxPoint(standardPin.pos.x + m_grid, standardPin.pos.y - m_grid);
    }
    clone.SetPos(standardpos);
    m_previewElement = clone;*/
    Refresh();
}

void CanvasPanel::WireSetWholeOffSet(int index, const wxPoint& offset) {
    for (auto& cp : m_wires[index].pts) {
        cp.pos += offset;
    }
    m_wires[index].GenerateCells();
}

void CanvasPanel::WirePtsSetPos(int wireIndex, int controlPointIndex, const wxPoint& pos) {
    m_wires[wireIndex].pts[controlPointIndex].pos = pos;
    m_wires[wireIndex].GenerateCells();
    Refresh();
    //触发改变
    SetModified(1);
}


void CanvasPanel::UpdateSelection(std::vector<int> m_elemIdx, std::vector<int> m_textIdx, std::vector<int> m_wireIdx) {
    m_selTxtIdx = m_textIdx;
    m_selElemIdx = m_elemIdx;
    m_selWireIdx = m_wireIdx;
    Refresh();
}

void CanvasPanel::AddWire(const Wire& wire) {
    m_wires.push_back(wire);
    m_wires.back().GenerateCells();
    auto& w = m_wires.back();

    int from_id = w.Left.elemIdx;
    int from_pin = w.Left.PinIdx;
    bool is_input = w.Left.isInput;
    while (from_pin == -1) {
        from_id = m_wires[w.Left.wireIdx].Left.elemIdx;
        from_pin = m_wires[w.Left.wireIdx].Left.PinIdx;
        is_input = m_wires[w.Left.wireIdx].Left.isInput;
    }


    int to_id = w.Right.elemIdx;
    int to_pin = w.Right.PinIdx;
    while (to_pin == -1) {
        to_id = m_wires[w.Right.wireIdx].Right.elemIdx;
        to_pin = m_wires[w.Right.wireIdx].Right.PinIdx;
    }

    TopNode* tn = m_tbox.self;
    if (from_id == -1 && to_id != -1) {
        SecondElement to = m_elems[to_id];
        tn->SetSecondPortConn(to.self, to.self->in_ports[to_pin].identifier, tn->in_ports[from_pin]->identifier);
        w.SetSelf(tn->in_ports[from_pin]);
        wxCommandEvent evt(EVT_SIGFLOWNODE_CHANGED);
        evt.SetClientData(to.self);
        m_parent->GetEventHandler()->ProcessEvent(evt);
    }
    else if (from_id != -1 && to_id == -1) {
        SecondElement from = m_elems[from_id];
        tn->SetSecondPortConn(from.self, from.self->out_ports[from_pin].identifier, tn->out_ports[to_pin]->identifier);
        w.SetSelf(tn->out_ports[to_pin]);
        wxCommandEvent evt(EVT_SIGFLOWNODE_CHANGED);
        evt.SetClientData(from.self);
        m_parent->GetEventHandler()->ProcessEvent(evt);
    }
    else if (from_id == -1 && to_id == -1){

    }
    else {
        SecondElement from = m_elems[from_id];
        SecondElement to = m_elems[to_id];

        SignalNode* sn = nullptr;
        if (!is_input) {
            sn = from.self->out_ports[from_pin].signal;
            if (!sn) sn =to.self->in_ports[to_pin].signal;
        }
        else {
            sn = from.self->in_ports[from_pin].signal;
            if (!sn) sn = to.self->out_ports[to_pin].signal;
        }

        if (!sn) sn = sftree->AddNewWire(tn);
        wxCommandEvent evt2(EVT_SIGFLOWNODE_CHANGED);
        evt2.SetClientData(sn);
        m_parent->GetEventHandler()->ProcessEvent(evt2);
        w.SetSelf(sn);

        if (is_input) {
            tn->SetSecondPortConn(from.self, from.self->in_ports[from_pin].identifier, sn->identifier);
            //from.self->in_ports[from_pin].conn = sn->identifier;
            wxCommandEvent evt0(EVT_SIGFLOWNODE_CHANGED);
            evt0.SetClientData(from.self);
            m_parent->GetEventHandler()->ProcessEvent(evt0);

            tn->SetSecondPortConn(to.self, to.self->out_ports[to_pin].identifier, sn->identifier);
            //to.self->out_ports[to_pin].conn = sn->identifier;
            wxCommandEvent evt(EVT_SIGFLOWNODE_CHANGED);
            evt.SetClientData(to.self);
            m_parent->GetEventHandler()->ProcessEvent(evt);
        }
        else {
            tn->SetSecondPortConn(from.self, from.self->out_ports[from_pin].identifier, sn->identifier);
            //from.self->out_ports[from_pin].conn = sn->identifier;
            wxCommandEvent evt0(EVT_SIGFLOWNODE_CHANGED);
            evt0.SetClientData(from.self);
            m_parent->GetEventHandler()->ProcessEvent(evt0);

            tn->SetSecondPortConn(to.self, to.self->in_ports[to_pin].identifier, sn->identifier);
            //to.self->in_ports[to_pin].conn = sn->identifier;
            wxCommandEvent evt(EVT_SIGFLOWNODE_CHANGED);
            evt.SetClientData(to.self);
            m_parent->GetEventHandler()->ProcessEvent(evt);
        }
    }
    


    Refresh();
    //触发改变
    SetModified(1);
};

void CanvasPanel::AddWireWithoutRecord(const Wire& wire) {
    m_wires.push_back(wire);
    m_wires.back().GenerateCells();
    Refresh();
};

void CanvasPanel::ReclaimWire(Wire wire, int index) {
    m_wires.insert(m_wires.begin() + index, std::move(wire));
    m_wires[index].GenerateCells();
    Refresh();
};

void CanvasPanel::DeleteWire(int index) {
    sftree->RemoveSignal(tn, m_wires[index].GetSelf());
    m_wires.erase(m_wires.begin() + index);
    m_selWireIdx.erase(std::remove(m_selWireIdx.begin(), m_selWireIdx.end(), index), m_selWireIdx.end());
    Refresh();
}


bool CanvasPanel::IsNear(const wxPoint& a, const wxPoint& b, int tol) {
    return std::abs(a.x - b.x) <= tol && std::abs(a.y - b.y) <= tol;
}

void CanvasPanel::SetTopNode(TopNode* node) {
    tn = node;
    UpdateCanvasElements();
    LoadLayout();
    CompleteAutoWiring();
    Refresh();
}

void CanvasPanel::LoadLayout() {
    if (Read()) return;
    CompleteAutoLayout();
}

void CanvasPanel::CompleteAutoWiring() {
    if (!tn || m_elems.empty()) return;
    m_wires.clear();

    const int G = m_grid;                          // 网格尺寸 (20)
    const int numElems = static_cast<int>(m_elems.size());

    // ═══════════════════════════════════════════════
    //  STEP 1  拓扑分层
    // ═══════════════════════════════════════════════
    std::vector<int> topoLevels = SigFlowTree::SecondNodeTopoLevel(tn);
    if (static_cast<int>(topoLevels.size()) != numElems) return;

    int maxLevel = 0;
    for (int l : topoLevels) maxLevel = std::max(maxLevel, l);

    // ═══════════════════════════════════════════════
    //  STEP 2  构建信号网表
    // ═══════════════════════════════════════════════
    struct Dest { int elemIdx; int pinIdx; int layer; };
    struct Net  {
        std::string name;
        int srcElem  = -2;       // -2 未设定, -1 顶层输入端口, ≥0 元件
        int srcPin   = 0;
        int srcLayer = -1;       // -1 表示顶层输入
        std::vector<Dest> dests;
    };
    std::map<std::string, Net> nets;

    // 2a  元件输出引脚 → 信号驱动源
    for (int i = 0; i < numElems; i++) {
        auto* sn = m_elems[i].self; if (!sn) continue;
        for (int p = 0; p < static_cast<int>(sn->out_ports.size()); p++) {
            const auto& sig = sn->out_ports[p].conn;
            if (sig.empty()) continue;
            auto& net    = nets[sig];
            net.name     = sig;
            net.srcElem  = i;
            net.srcPin   = p;
            net.srcLayer = topoLevels[i];
        }
    }
    // 2b  顶层模块输入端口 → 信号驱动源
    auto& inPorts = tn->GetInPorts();
    for (int p = 0; p < static_cast<int>(inPorts.size()); p++) {
        const auto& sig = inPorts[p]->identifier;
        auto& net    = nets[sig];
        net.name     = sig;
        net.srcElem  = -1;
        net.srcPin   = p;
        net.srcLayer = -1;
    }
    // 2c  元件输入引脚 → 信号消费者
    for (int i = 0; i < numElems; i++) {
        auto* sn = m_elems[i].self; if (!sn) continue;
        for (int p = 0; p < static_cast<int>(sn->in_ports.size()); p++) {
            const auto& sig = sn->in_ports[p].conn;
            if (sig.empty() || !nets.count(sig)) continue;
            nets[sig].dests.push_back({i, p, topoLevels[i]});
        }
    }
    // 2d  顶层模块输出端口 → 信号消费者
    auto& outPorts = tn->GetOutPorts();
    for (int p = 0; p < static_cast<int>(outPorts.size()); p++) {
        const auto& sig = outPorts[p]->identifier;
        if (!nets.count(sig)) continue;
        nets[sig].dests.push_back({-1, p, maxLevel + 1});
    }
    // 2e  清除无驱动源或无消费者的网络
    for (auto it = nets.begin(); it != nets.end(); )
        (it->second.srcElem == -2 || it->second.dests.empty())
            ? it = nets.erase(it) : ++it;
    if (nets.empty()) return;

    // ═══════════════════════════════════════════════
    //  STEP 3  计算布线通道需求
    // ═══════════════════════════════════════════════
    // Gap[g] 位于 layer[g-1] 与 layer[g] 之间的垂直通道区
    //   Gap[0]            : 顶层输入侧 ↔ layer[0]
    //   Gap[maxLevel+1]   : layer[maxLevel] ↔ 顶层输出侧
    const int numGaps = maxLevel + 2;
    std::vector<std::vector<std::string>> gapSigs(numGaps);
    std::vector<std::string> hChanSigs;        // 需要水平通道的信号

    for (auto& [sig, net] : nets) {
        int farthest = -1;
        for (auto& d : net.dests) farthest = std::max(farthest, d.layer);
        int gStart = std::max(0, net.srcLayer + 1);
        int gEnd   = std::min(numGaps - 1, farthest);
        for (int g = gStart; g <= gEnd; g++)
            gapSigs[g].push_back(sig);
        if (farthest - net.srcLayer > 1)
            hChanSigs.push_back(sig);
    }
    int numHChans = static_cast<int>(hChanSigs.size());

    // ═══════════════════════════════════════════════
    //  STEP 4  带通道预留的自动布局
    // ═══════════════════════════════════════════════
    // 4a  每层最大元件宽度
    std::map<int, int> layerMaxW;
    for (int i = 0; i < numElems; i++)
        layerMaxW[topoLevels[i]] = std::max(
            layerMaxW[topoLevels[i]],
            m_elems[i].GetBounds().GetWidth());

    // 4b  水平坐标: [tbLeft] [gap0] [layer0] [gap1] [layer1] … [gapN+1] [tbRight]
    const int tbLeft = 3 * G;
    const int tbTop  = 3 * G;
    int curX = tbLeft + 2 * G;

    std::vector<int> gapX(numGaps, 0);
    std::vector<int> gapW(numGaps, 0);
    std::vector<int> layerX(maxLevel + 1, 0);

    for (int lev = 0; lev <= maxLevel; lev++) {
        gapX[lev] = curX;
        gapW[lev] = std::max(1, static_cast<int>(gapSigs[lev].size())) * G;
        curX += gapW[lev] + G;
        layerX[lev] = curX;
        curX += layerMaxW[lev] + G;
    }
    int lastG = numGaps - 1;
    gapX[lastG] = curX;
    gapW[lastG] = std::max(1, static_cast<int>(gapSigs[lastG].size())) * G;
    curX += gapW[lastG] + 2 * G;
    const int tbRight = curX;

    // 4c  垂直坐标: 水平通道在上方, 元件在下方
    const int hChanStartY = tbTop + 2 * G;
    const int hChanH      = numHChans * G;
    const int elemStartY  = hChanStartY + hChanH + (numHChans > 0 ? G : 0);

    std::map<int, int> layerCurY;
    for (int i = 0; i < numElems; i++) {
        int lev = topoLevels[i];
        if (!layerCurY.count(lev)) layerCurY[lev] = elemStartY;
        m_elems[i].SetPos(wxPoint(layerX[lev], layerCurY[lev]));
        layerCurY[lev] += m_elems[i].GetBounds().GetHeight() + 3 * G;
}

    int maxBotY = elemStartY;
    for (auto& [_, y] : layerCurY) maxBotY = std::max(maxBotY, y);
    const int tbBottom = maxBotY + 2 * G;

    // 4d  重建 TopModuleBox
    m_tbox = TopModuleBox(wxPoint(tbLeft, tbTop),
                          wxPoint(tbRight, tbBottom), tn);

    // ═══════════════════════════════════════════════
    //  STEP 5  分配通道坐标
    // ═══════════════════════════════════════════════
    // 5a  垂直通道：按 (源层降序, 源Y升序) 分配
    //     邻层信号靠近目标侧 → 减少交叉
    std::map<std::string, std::map<int, int>> vChanX;

    for (int g = 0; g < numGaps; g++) {
        auto& sigs = gapSigs[g];
        std::sort(sigs.begin(), sigs.end(),
            [&](const std::string& a, const std::string& b) {
                auto& nA = nets[a]; auto& nB = nets[b];
                if (nA.srcLayer != nB.srcLayer)
                    return nA.srcLayer > nB.srcLayer;
                auto srcY = [&](const Net& n) -> int {
                    if (n.srcElem >= 0) {
                        auto& e = m_elems[n.srcElem];
                        auto& pins = e.GetOutputPins();
                        return (n.srcPin < (int)pins.size())
                            ? e.GetPos().y + pins[n.srcPin].pos.y : 0;
                    }
                    auto& pins = m_tbox.GetInputPins();
                    return (n.srcPin < (int)pins.size())
                        ? m_tbox.GetPos().y + pins[n.srcPin].pos.y : 0;
                };
                return srcY(nA) < srcY(nB);
            });
        for (int ch = 0; ch < static_cast<int>(sigs.size()); ch++)
            vChanX[sigs[ch]][g] = gapX[g] + ch * G;
    }

    // 5b  水平通道：从上到下依次分配
    std::map<std::string, int> hChanY;
    for (int ch = 0; ch < numHChans; ch++)
        hChanY[hChanSigs[ch]] = hChanStartY + ch * G;

    // ═══════════════════════════════════════════════
    //  STEP 6  生成导线
    // ═══════════════════════════════════════════════

    /*
    auto srcPinPos = [&](const Net& n) -> wxPoint {
        if (n.srcElem == -1) {
            auto& pins = m_tbox.GetInputPins();
            return (n.srcPin < (int)pins.size())
                ? m_tbox.GetPos() + wxPoint(pins[n.srcPin].pos.x, pins[n.srcPin].pos.y)
                : wxPoint(0, 0);
        }
        auto& e = m_elems[n.srcElem];
        auto& pins = e.GetOutputPins();
        return (n.srcPin < (int)pins.size())
            ? e.GetPos() + wxPoint(pins[n.srcPin].pos.x, pins[n.srcPin].pos.y)
            : wxPoint(0, 0);
    };

    auto dstPinPos = [&](const Dest& d) -> wxPoint {
        if (d.elemIdx == -1) {
            auto& pins = m_tbox.GetOutputPins();
            return (d.pinIdx < (int)pins.size())
                ? m_tbox.GetPos() + wxPoint(pins[d.pinIdx].pos.x, pins[d.pinIdx].pos.y)
                : wxPoint(0, 0);
        }
        auto& e = m_elems[d.elemIdx];
        auto& pins = e.GetInputPins();
        return (d.pinIdx < (int)pins.size())
            ? e.GetPos() + wxPoint(pins[d.pinIdx].pos.x, pins[d.pinIdx].pos.y)
            : wxPoint(0, 0);
    };

    auto getChanX = [&](const std::string& sig, int g) -> int {
        auto it1 = vChanX.find(sig);
        if (it1 != vChanX.end()) {
            auto it2 = it1->second.find(g);
            if (it2 != it1->second.end()) return it2->second;
        }
        return gapX[g];
    };

    // ─── 去除公共前缀，拆分为主干线 + 分支线 ───
    auto cleanPath = [](const std::vector<ControlPoint>& pts) {
        std::vector<ControlPoint> clean;
        for (auto& p : pts)
            if (clean.empty() || clean.back().pos != p.pos)
                clean.push_back(p);
        return clean;
    };

    auto addWiresForPaths = [&](auto& self,
        const std::vector<std::vector<ControlPoint>>& allPaths) -> void
    {
        if (allPaths.empty()) return;

        if (allPaths.size() == 1) {
            auto clean = cleanPath(allPaths[0]);
            if (static_cast<int>(clean.size()) >= 2) {
                Wire w(std::move(clean));
                w.m_canvas = this;
                m_wires.push_back(std::move(w));
            }
            return;
        }

        int minLen = static_cast<int>(allPaths[0].size());
        for (size_t i = 1; i < allPaths.size(); i++)
            minLen = std::min(minLen, static_cast<int>(allPaths[i].size()));

        int prefixLen = 0;
        for (int i = 0; i < minLen; i++) {
            bool allSame = true;
            for (size_t j = 1; j < allPaths.size(); j++) {
                if (allPaths[j][i].pos != allPaths[0][i].pos) {
                    allSame = false; break;
                }
            }
            if (!allSame) break;
            prefixLen++;
        }

        if (prefixLen < 2) {
            for (auto& path : allPaths) {
                auto clean = cleanPath(path);
                if (static_cast<int>(clean.size()) >= 2) {
                    Wire w(std::move(clean));
                    w.m_canvas = this;
                    m_wires.push_back(std::move(w));
                }
            }
            return;
        }

        // 主干线：从源引脚到分支点
        std::vector<ControlPoint> trunk(
            allPaths[0].begin(), allPaths[0].begin() + prefixLen);
        trunk.back().type = CPType::Branch;
        auto cleanTrunk = cleanPath(trunk);
        if (static_cast<int>(cleanTrunk.size()) >= 2) {
            cleanTrunk.back().type = CPType::Branch;
            Wire w(std::move(cleanTrunk));
            w.m_canvas = this;
            m_wires.push_back(std::move(w));
        }

        // 按分叉方向分组，递归处理子路径
        wxPoint branchPos = allPaths[0][prefixLen - 1].pos;
        std::map<std::pair<int,int>,
                 std::vector<std::vector<ControlPoint>>> groups;

        for (auto& path : allPaths) {
            std::vector<ControlPoint> sub;
            sub.push_back({branchPos, CPType::Branch});
            for (int i = prefixLen; i < static_cast<int>(path.size()); i++)
                sub.push_back(path[i]);

            auto key = (prefixLen < static_cast<int>(path.size()))
                ? std::make_pair(path[prefixLen].pos.x, path[prefixLen].pos.y)
                : std::make_pair(path.back().pos.x, path.back().pos.y);
            groups[key].push_back(std::move(sub));
        }

        for (auto& [k, subPaths] : groups)
            self(self, subPaths);
    };

    for (auto& [sig, net] : nets) {
        wxPoint sp = srcPinPos(net);
        std::vector<std::vector<ControlPoint>> forwardPaths;

        for (auto& dest : net.dests) {
            SignalNode* sn = new SignalNode("Unknows", SignalType::Wire);
            if (dest.elemIdx != -1 && dest.pinIdx != -1) {

                Pin p = m_elems[dest.elemIdx].GetInputPins()[dest.pinIdx];
                
                if (p.isOfTop()) {
                    sn = p.GetSelfSignal();
                }
                else {
                    SignalNode* n = p.GetSelfPort()->signal;
                    if (n) sn = p.GetSelfPort()->signal;
                }
            }





            wxPoint dp = dstPinPos(dest);
            int dL = dest.layer;
            bool isSkip = (dL - net.srcLayer) > 1;

            if (dL <= net.srcLayer) {
                std::vector<ControlPoint> pts = {{sp, CPType::Pin}, {dp, CPType::Pin}};
                auto clean = cleanPath(pts);
                if (static_cast<int>(clean.size()) >= 2) {
                    Wire w(std::move(clean));
                    w.m_canvas = this;
                    m_wires.push_back(std::move(w));
                }
                continue;
            }

            std::vector<ControlPoint> pts;
            if (!isSkip) {
                int cx = getChanX(sig, dL);
                pts.push_back({sp, CPType::Pin});
                if (sp.y != dp.y) {
                    pts.push_back({{cx, sp.y}, CPType::Bend});
                    pts.push_back({{cx, dp.y}, CPType::Bend});
                } else {
                    pts.push_back({{cx, sp.y}, CPType::Bend});
                }
                pts.push_back({dp, CPType::Pin});
            }
            else {
                int firstG = std::max(0, net.srcLayer + 1);
                int cx1 = getChanX(sig, firstG);
                int cx2 = getChanX(sig, dL);
                int hy  = hChanY.count(sig) ? hChanY[sig] : hChanStartY;
                pts.push_back({sp, CPType::Pin});
                pts.push_back({{cx1, sp.y}, CPType::Bend});
                pts.push_back({{cx1, hy},   CPType::Bend});
                pts.push_back({{cx2, hy},   CPType::Bend});
                pts.push_back({{cx2, dp.y}, CPType::Bend});
                pts.push_back({dp, CPType::Pin});
            }

            // 移除零长线段
            
            std::vector<ControlPoint> clean;
            for (auto& p : pts)
                if (clean.empty() || clean.back().pos != p.pos)
                    clean.push_back(p);

            if (static_cast<int>(clean.size()) >= 2) {
                Wire w(std::move(clean));
                w.SetSelf(sn);
                w.m_canvas = this;
                m_wires.push_back(std::move(w));
            }
            auto clean = cleanPath(pts);
            if (static_cast<int>(clean.size()) >= 2)
                forwardPaths.push_back(std::move(clean));
        }
        addWiresForPaths(addWiresForPaths, forwardPaths);
    }

    for (auto& w : m_wires) w.GenerateCells();*/

    Refresh();
}

void CanvasPanel::CompleteAutoLayout() {
    if (m_elems.empty()) {
        m_tbox = TopModuleBox(wxPoint(60, 40), wxPoint(700, 440), tn);
    }
    else {
        std::vector<SecondElement>& elems = m_elems;
        std::vector<int> topoLevels = SigFlowTree::SecondNodeTopoLevel(tn);
        std::unordered_map<int, int> levelBounds = GetLevelBounds(topoLevels, &elems);

        std::unordered_map<int, int> levelCurrentY;
        std::unordered_map<int, int> levelStartX;

        wxPoint start = wxPoint(60, 40);

        int currentX = start.x + 100; // 画布初始左边距
        int totalLevels = 0;
        for (auto const& [lev, width] : levelBounds) {
            totalLevels = std::max(totalLevels, lev);
        }

        for (int l = 0; l <= totalLevels; ++l) {
            if (levelBounds.count(l)) {
                levelStartX[l] = currentX;
                currentX += levelBounds[l] + 100;
            }
        }

        for (size_t i = 0; i < topoLevels.size(); ++i) {
            int lev = topoLevels[i];
            SecondElement& ce = elems[i];

            // 初始化该层的 Y 坐标起始值
            if (levelCurrentY.find(lev) == levelCurrentY.end()) {
                levelCurrentY[lev] = start.y + 40; // 画布初始上边距
            }

            // 获取当前元件的尺寸（从 GetBounds 中获取）
            wxRect rect = ce.GetBounds();

            // 计算位置：
            // X 使用预存的该层起始 X
            // Y 使用该层当前的累计 Y
            wxPoint targetPos(levelStartX[lev], levelCurrentY[lev]);
            ce.SetPos(targetPos);

            // 更新该层下一元件的 Y 坐标：当前高度 + 元件自身高度 + 间隔20
            levelCurrentY[lev] += rect.GetHeight() + 60;
        }
        int maxY = 0;
        if (!levelCurrentY.empty()) {
            auto it = std::max_element(levelCurrentY.begin(), levelCurrentY.end(),
                [](const std::pair<int, int>& a, const std::pair<int, int>& b) {
                    return a.second < b.second; // 比较 value
                });

            maxY = it->second;
            // 如果需要对应的层级(Key): int maxKey = it->first;
        }
        wxPoint end = wxPoint(levelStartX[totalLevels] + levelBounds[totalLevels] + 100, maxY + 80);
        m_tbox = TopModuleBox(start, end, tn);
    }
    
}

void CanvasPanel::UpdateCanvasElements(){
    m_elems = GetSecondElements(tn);
}

std::vector<SecondElement> CanvasPanel::GetSecondElements(TopNode* n) {
    extern std::vector<SecondElement> g_elements;
    std::vector<SecondElement> elems;

    for (auto* child : n->GetChildren())
    {
        if (child->type == SigTreeNodeType::Signal) continue;
        SecondNode* sn = static_cast<SecondNode*>(child);
        elems.push_back(SecondElement(sn, g_elements));
    }
    return elems;
}

std::unordered_map<int, int> CanvasPanel::GetLevelBounds(std::vector<int> topoLevels, std::vector<SecondElement>* p_elems) {
    if (!p_elems || topoLevels.size() != p_elems->size()) {
        return {};
    }

    // Key: level ID
    // Value: 存储该层所有元件的最左端和最右端坐标
    struct Limit { int minX = INT_MAX; int maxX = INT_MIN; };
    std::unordered_map<int, Limit> levelLimits;

    for (size_t i = 0; i < topoLevels.size(); ++i) {
        int level = topoLevels[i];
        const SecondElement& ce = (*p_elems)[i];

        // 使用你提到的 GetBounds() 获取元件在画布上的实际矩形区域
        wxRect bounds = ce.GetBounds();

        // 更新该层级的水平极值
        if (bounds.GetLeft() < levelLimits[level].minX)
            levelLimits[level].minX = bounds.GetLeft();

        if (bounds.GetRight() > levelLimits[level].maxX)
            levelLimits[level].maxX = bounds.GetRight();
    }

    // 转换为最终的宽度结果
    std::unordered_map<int, int> resultWidths;
    for (auto const& [level, limit] : levelLimits) {
        // 宽度 = 最大 X - 最小 X
        // 注意：如果该层只有一个元件，宽度就是该元件自身的宽度
        resultWidths[level] = limit.maxX - limit.minX;
    }

    return resultWidths;
}

wxString CanvasPanel::GetNote() {
    if (tn) return tn->identifier;
    else return wxString("untitled");
}


void CanvasPanel::AddGateNode(GateType type, wxPoint pos) {
    extern std::vector<SecondElement> g_elements;

    GateInstNode* gn = new GateInstNode("new_"+SigFlowTree::ToString(type), type);
    gn = static_cast<GateInstNode*>(sftree->AddChild(tn, gn));
    Refresh();
}

void CanvasPanel::AddModuleInstNode(wxString def, wxPoint pos) {
    extern std::vector<SecondElement> g_elements;
    ModuleInstNode* mn = new ModuleInstNode("new_"+ def.ToStdString(), def.ToStdString());

    mn = static_cast<ModuleInstNode*>(sftree->AddChild(tn, mn));
    Refresh();
}

void CanvasPanel::AddSecondElement(SecondNode* sn) {
    extern std::vector<SecondElement> g_elements;
    SecondElement s = SecondElement(sn, g_elements);
    if (m_previewElement.GetIdentifier() == s.GetIdentifier()) {
        s.SetPos(m_previewElement.GetPos());
    }
    else  s.SetPos(wxPoint(0, 0));
    m_elems.push_back(s);
    RefreshRect(s.GetBounds());
    //触发改变
    SetModified(1);
}

void CanvasPanel::AddSecondNode(wxString type, wxPoint pos) {
    GateType gt = SigFlowTree::GateTypeFromString(type.ToStdString());
    if (gt != GateType::Unknown) AddGateNode(gt, pos);
    else AddModuleInstNode(type, pos);
}

void CanvasPanel::SetPreview(wxString type) {
    GateType gt = SigFlowTree::GateTypeFromString(type.ToStdString());
    if (gt != GateType::Unknown) {
        extern std::vector<SecondElement> g_elements;

        GateInstNode* gn = new GateInstNode("new_" + SigFlowTree::ToString(gt), gt);

        SecondElement se = SecondElement(gn, g_elements);
        se.SetPos(wxPoint(0, 0));
        m_previewElement = se;
        RefreshRect(se.GetBounds());
    }
    else {
        extern std::vector<SecondElement> g_elements;
        ModuleInstNode* mn = new ModuleInstNode("new_" + type.ToStdString(), type.ToStdString());
        sftree->LinkSingleInstWithDef(mn);
        if (mn->Definition) {
            wxCommandEvent evt;
            ProcessWindowEvent(evt);

            SecondElement se = SecondElement(mn, g_elements);
            se.SetPos(wxPoint(0, 0));
            m_previewElement = se;
            Refresh();
        }
        else {

        }
    };
}


void CanvasPanel::SetPreviewPos(wxPoint pos) {
    m_previewElement.SetPos(pos);
    RefreshRect(m_previewElement.GetBounds());
}

void CanvasPanel::DelSecondNode(int id) {
    SecondElement& sm = m_elems[id];
    SecondNode* sn = sm.self;
    sftree->RemoveChild(sn->GetParent(), sn);
}

void CanvasPanel::DelSecondElement(SecondNode* sn) {
    // 1. 查找要删除元素的索引
    int targetIdx = -1;
    for (int i = 0; i < m_elems.size(); ++i) {
        if (m_elems[i].self == sn) {
            targetIdx = i;
            break;
        }
    }

    if (targetIdx == -1) return; // 没找到，直接返回

    // 2. 清理选中索引 vector (m_selElemIdx)
    // 移除等于 targetIdx 的索引
    m_selElemIdx.erase(
        std::remove(m_selElemIdx.begin(), m_selElemIdx.end(), targetIdx),
        m_selElemIdx.end()
    );

    // 3. 将所有大于 targetIdx 的索引减 1 (因为元素被删了，后面的元素索引前移)
    for (int& idx : m_selElemIdx) {
        if (idx > targetIdx) {
            idx--;
        }
    }

    // 4. 最后删除 m_elems 中的元素
    m_elems.erase(m_elems.begin() + targetIdx);
    //触发改变
    SetModified(1);
}

void CanvasPanel::DelSecondElement(int id) {
    // 1. 安全检查：确保索引在有效范围内
    if (id < 0 || id >= (int)m_elems.size()) {
        return;
    }

    // 2. 清理选中索引数组 (m_selElemIdx)
    // 2a. 移除等于该 id 的索引 (如果该元素当前被选中)
    m_selElemIdx.erase(
        std::remove(m_selElemIdx.begin(), m_selElemIdx.end(), id),
        m_selElemIdx.end()
    );

    // 2b. 将所有大于该 id 的索引减 1
    for (int& selIdx : m_selElemIdx) {
        if (selIdx > id) {
            selIdx--;
        }
    }

    // 3. 删除 m_elems 中的元素
    m_elems.erase(m_elems.begin() + id);
}
void CanvasPanel::SetModified(bool modified) {
    if (m_isModified == modified) return; // 避免重复触发
    m_isModified = modified;

    // 初始化画布标识（用 GetNote()，即 tn->identifier）
    if (m_canvasId.IsEmpty()) {
        m_canvasId = GetNote();
    }

    if (modified) { // 仅标记为修改时发事件
        wxCommandEvent evt(wxEVT_CANVAS_MODIFIED);
        evt.SetString(m_canvasId); // 附带画布标识（关键）
        evt.SetEventObject(this);  // 附带当前画布对象
        wxPostEvent(GetParent(), evt); // 发给父窗口 CanvasNoteBook
    }
}

void CanvasPanel::RefreshElem(SecondNode* sn) {
    extern std::vector<SecondElement> g_elements;
    for (auto& elem : m_elems) {
        if (elem.self == sn) {
            auto tmp = SecondElement(sn, g_elements);
            tmp.SetPos(elem.GetPos());
            elem = tmp;
        }
    }
}

void CanvasPanel::RefreshSignal(SignalNode* sn) {
    for (auto& w : m_wires) {
        if (w.GetSelf() == sn) {
            w.SetSelf(sn);
        }
    }
}

void CanvasPanel::OnSFNodeActivated(wxCommandEvent& evt) {
    // 透传事件到父窗口（CanvasNoteBook）
    if (m_mainFrame != nullptr) { // m_mainFrame 是 CanvasPanel 中指向 CanvasNoteBook 的指针
        wxPostEvent(m_mainFrame, evt);
    }
    evt.Skip(); // 允许事件继续传播
}
