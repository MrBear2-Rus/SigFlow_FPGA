#include <wx/graphics.h> 
#include "platform/PlatformPaths.h"
#include <wx/dcbuffer.h>
#include <wx/dcgraph.h>  
#include <wx/filename.h>
#include <wx/file.h>
#include <wx/stdpaths.h>
#include <fstream>
#include <filesystem>
#include <algorithm>
#include <unordered_set>



#include "CanvasPanel.h"
#include "my_log.h"
#include "json.hpp"

#include "ToolStateMachine.h"
#include "HandyToolKit.h"
#include "CanvasEventHandler.h"
#include "CanvasNoteBook.h"
#include "ui/UiTheme.h"   // T-410：画布唯一取色来源（SAFE-012 / THE-001）
#include "ui/UiFocusTrace.h"  // T-620：键盘输入来源（A11Y-001 / DEC-14）
#include "ui/CanvasVisuals.h"
#include "ui/CanvasControls.h"
#include "MainFrame.h"

namespace {

// wxString → UTF-8 std::string。
//
// 绝不能用 wxString::ToStdString()：它走的是当前 locale（Windows 上是 ANSI/CP936），
// 于是中文元件名/画布名会变成非法 UTF-8 字节：
//   * 交给 nlohmann 的 dump() 会抛 type_error.316，而 Save() 没有 try/catch，
//     异常逃出 UI 事件处理就是 std::terminate（整个进程被终止）；
//   * 在 Linux 的非 UTF-8 locale 下还会静默变成空串（数据丢失）。
std::string ToUtf8(const wxString& text)
{
    const wxScopedCharBuffer buffer = text.ToUTF8();
    return std::string(buffer.data() != nullptr ? buffer.data() : "", buffer.length());
}

} // namespace

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
    // T-620 / INT-019：右键召出的自绘快捷工具浮层自己处理 Esc（关闭浮层、焦点回画布、
    // 不改动任何业务状态）。实现文件 HandyToolKit.* 不在本轮允许修改范围内，
    // 因此在画布侧给它挂一个按键处理器，只消费 Esc。
    m_HandyToolKit->Bind(wxEVT_KEY_DOWN, [this](wxKeyEvent& event) {
        if (event.GetKeyCode() != WXK_ESCAPE) { event.Skip(); return; }
        UiFocusTrace::NoteKeyboard();
        if (m_HandyToolKit->IsShown()) m_HandyToolKit->Hide();
        SetFocus();
    });

    m_CanvasEventHandler->SetCurrentTool(ToolType::SELECT_TOOL);
    m_floatToolbar = new CanvasControls::FloatToolbar(this,[this](ToolType tool){ActivateCanvasTool(tool);});
    m_zoomHud = new CanvasControls::ZoomHud(this,[this](int direction){StepZoom(direction);},[this]{ResetView();});
    m_toolStateMachine->Bind(TOOL_STATE_CHANGED,[this](wxCommandEvent& e){SyncCanvasControls();e.Skip();});
    Bind(wxEVT_SIZE,[this](wxSizeEvent& e){LayoutCanvasControls();e.Skip();});

    SetBackgroundStyle(wxBG_STYLE_PAINT);
    // T-410：原 *wxWHITE → canvas token（浅色档同为 #ffffff，深色档随主题收敛）
    SetBackgroundColour(UiTheme::Palette().canvas);
    m_themeSubscription = UiTheme::Subscribe([this] {
        SetBackgroundColour(UiTheme::Palette().canvas);
        Refresh(false); // 只在主题通知时刷新，不从 OnPaint 发起重绘。
    });
    Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent& e) {
        m_pointerInside = false;
        if (m_toolStateMachine->IsPlacingComponent()) Refresh(false);
        e.Skip();
    });


    Bind(wxEVT_KEY_UP, [this](wxKeyEvent& e) {
        if (e.GetKeyCode()==WXK_SPACE && m_spacePan) {
            m_spacePan=false;
            if (m_pointerPan) FinishPointerGesture();
        } else e.Skip();
    });
    Bind(wxEVT_MIDDLE_DOWN, [this](wxMouseEvent& e) {
        if (m_currentEditingTextIndex<0) { EnsureFocus(); BeginPointerPan(e.GetPosition(),true); }
        else e.Skip();
    });
    Bind(wxEVT_MIDDLE_UP, [this](wxMouseEvent& e) {
        if (m_middlePan) FinishPointerGesture(); else e.Skip();
    });
    Bind(wxEVT_MOUSE_CAPTURE_LOST, [this](wxMouseCaptureLostEvent&) {
        m_spacePan=false; m_pendingPlace=false; m_pointerPan=false; m_middlePan=false;
        m_toolStateMachine->SetSelectState(SelectToolState::IDLE);
        SetCursor(m_gestureCursor.IsOk()?m_gestureCursor:wxCursor(wxCURSOR_ARROW));
    });
    Bind(wxEVT_DPI_CHANGED, [this](wxDPIChangedEvent& e) {
        for (auto& text:m_textElements) text.SetText(text.GetText());
        LayoutCanvasControls();
        Refresh(false); e.Skip();
    });
    SetScale(1.0);
    SetoffSet(wxPoint(0, 0));
    LayoutCanvasControls();SyncCanvasControls();
    SetFocus();
}

void CanvasPanel::OnLeftDown(wxMouseEvent& evt){
    EnsureFocus();
    m_HandyToolKit->Hide();
    UpdateHoverInfo(evt.GetPosition());
    if (m_spacePan || (m_toolStateMachine->GetCurrentTool()==ToolType::DRAG_TOOL &&
                       !IsOverPin() && !IsOverCell())) { BeginPointerPan(evt.GetPosition()); return; }
    if (m_toolStateMachine->GetCurrentTool()==ToolType::COMPONENT_TOOL) {
        if (m_placingName.IsEmpty()) {SetStatus("Component: choose from Component Library");return;}
        m_pendingPlace=true; m_placeDragged=false; m_gestureStart=evt.GetPosition();
        if (!HasCapture()) CaptureMouse();
        return; // mouse-up 确认点击后仍调用原有放置业务入口。
    }
    m_CanvasEventHandler->OnCanvasLeftDown(evt);
    if ((m_toolStateMachine->GetCurrentTool()==ToolType::SELECT_TOOL ||
         m_toolStateMachine->GetWireState()==WireToolState::WIRE_DRAWING) && !HasCapture()) CaptureMouse();
}

void CanvasPanel::OnMouseMove(wxMouseEvent& evt) {
    m_pointerInside = true;
    if (m_pointerPan) {
        SetoffSet(m_panOffset+evt.GetPosition()-m_gestureStart);
        return;
    }
    if (m_pendingPlace) {
        const auto delta=evt.GetPosition()-m_gestureStart;
        if (std::hypot(delta.x,delta.y)>FromDIP(3)) m_placeDragged=true;
    }
    // T-340：回报网格吸附后的画布坐标，供主窗口状态栏显示（CMP-072）
    if (m_cursorReport) {
        m_cursorReport(Snap(ClientToCanvas(evt.GetPosition())));
    }

    UpdateHoverInfo(evt.GetPosition());

    m_CanvasEventHandler->OnCanvasMouseMove(evt);
    if (m_toolStateMachine->IsPlacingComponent()) {
        const auto p=GetSnappedPos();
        SetStatus(wxString::Format("Preview %s @ (%d, %d)",m_placingName,p.x,p.y));
    }
}

void CanvasPanel::OnLeftUp(wxMouseEvent& evt){
    if (m_pointerPan) { FinishPointerGesture(); return; }
    if (m_pendingPlace) {
        const auto delta=evt.GetPosition()-m_gestureStart;
        const bool place=!m_placeDragged && std::hypot(delta.x,delta.y)<=FromDIP(3) && GetClientRect().Contains(evt.GetPosition());
        const auto name=m_placingName;
        FinishPointerGesture();
        if (place && m_toolStateMachine->IsPlacingComponent()) {
            UpdateHoverInfo(evt.GetPosition());
            const auto p=GetSnappedPos();
            const auto count=m_elems.size();
            wxMouseEvent down(evt); down.SetEventType(wxEVT_LEFT_DOWN);
            m_CanvasEventHandler->OnCanvasLeftDown(down);
            if (m_elems.size()>count) SetStatus(wxString::Format("Placed %s @ (%d, %d)",name,p.x,p.y));
        }
        return;
    }
    UpdateHoverInfo(evt.GetPosition());
    m_CanvasEventHandler->OnCanvasLeftUp(evt);
    if (HasCapture()) ReleaseMouse();

}

void CanvasPanel::OnLeftDoubleClick(wxMouseEvent& evt) {
    m_CanvasEventHandler->OnCanvasLeftDoubleClick(evt);
}

void CanvasPanel::OnKeyDown(wxKeyEvent& evt) {
    // KEY-009：空格只在**画布自身**聚焦时表示临时平移。编辑器（SigTextEditor）聚焦时
    // 按键事件根本不会到达画布；画布上的文本编辑（hidden TextCtrl / m_currentEditingTextIndex）
    // 也会抢走焦点；这里再显式挡一次，保证「输入空格」永远不会被平移劫持。
    const bool textEditing = m_currentEditingTextIndex >= 0 ||
                             (m_hiddenTextCtrl != nullptr && m_hiddenTextCtrl->HasFocus());
    if (wxWindow::FindFocus() != this || textEditing) { evt.Skip(); return; }
    if (evt.GetKeyCode()==WXK_SPACE && !evt.ControlDown() && !evt.AltDown()) {
        m_spacePan=true; return;
    }
    if (evt.GetKeyCode()==WXK_ESCAPE && m_toolStateMachine->GetCurrentTool()==ToolType::COMPONENT_TOOL) {
        FinishPointerGesture(); SetCurrentTool(ToolType::SELECT_TOOL); Refresh(false); return;
    }
    m_CanvasEventHandler->OnCanvasKeyDown(evt);

}

void CanvasPanel::BeginPointerPan(const wxPoint& pos, bool middle)
{
    m_pointerPan=true; m_middlePan=middle; m_gestureStart=pos; m_panOffset=m_offset;
    m_gestureCursor=GetCursor(); SetCursor(wxCursor(wxCURSOR_HAND));
    if (!HasCapture()) CaptureMouse();
}

void CanvasPanel::FinishPointerGesture()
{
    if (m_pointerPan && m_gestureCursor.IsOk()) SetCursor(m_gestureCursor);
    m_pointerPan=false; m_middlePan=false; m_pendingPlace=false; m_placeDragged=false;
    if (HasCapture()) ReleaseMouse();
}

void CanvasPanel::OnMouseWheel(wxMouseEvent& evt) {
    if (m_pointerPan) return;
    if (!evt.GetWheelRotation()) return;
    if (evt.ShiftDown() || evt.GetWheelAxis()==wxMOUSE_WHEEL_HORIZONTAL) {
        const int delta=evt.GetWheelRotation()>0?FromDIP(40):-FromDIP(40);
        SetoffSet(m_offset+wxPoint(delta,0));
        return;
    }
    const double steps=double(evt.GetWheelRotation())/std::max(1,evt.GetWheelDelta());
    ZoomAt(m_scale*std::pow(1.12,steps),evt.GetPosition());
    UpdateHoverInfo(evt.GetPosition());

}

void CanvasPanel::SetScale(float scale) {
    float min_scale, max_scale;
    std::tie(min_scale, max_scale) = ValidScaleRange();
    if (scale < min_scale) scale = min_scale;
    if (scale > max_scale) scale = max_scale;
    m_scale = scale;
    if (m_zoomHud) m_zoomHud->Sync(m_scale);
    for (auto& text:m_textElements) text.SetPosition(text.GetPosition());
    // T-340：缩放变化回报给状态栏（CMP-072）
    if (m_scaleReport) {
        m_scaleReport(m_scale);
    }
    Refresh(); 
}

void CanvasPanel::ZoomAt(float scale,const wxPoint& anchor) {
    const auto old=m_scale;
    const auto offset=m_offset;
    SetScale(scale);
    const double ratio=m_scale/old;
    SetoffSet(wxPoint(std::lround(anchor.x+(offset.x-anchor.x)*ratio),
                     std::lround(anchor.y+(offset.y-anchor.y)*ratio)));
}

void CanvasPanel::StepZoom(int direction) {
    const auto size=GetClientSize();
    ZoomAt(m_scale+direction*0.25f,wxPoint(size.x/2,size.y/2));
    SetFocus();
}

void CanvasPanel::ResetView() {
    SetScale(1);SetoffSet(wxPoint(0,0));SetFocus();
}

void CanvasPanel::LayoutCanvasControls() {
    if (!m_floatToolbar || !m_zoomHud) return;
    const auto client=GetClientSize();
    const int margin=FromDIP(12);
    const int scroll=std::max(0,wxSystemSettings::GetMetric(wxSYS_VSCROLL_X));
    const int available=std::max(0,client.x-scroll);
    m_floatToolbar->FitWidth(available);
    m_zoomHud->FitWidth(available);
    m_floatToolbar->SetPosition(wxPoint(std::max(0,(available-m_floatToolbar->GetSize().x)/2),margin));
    m_zoomHud->SetPosition(wxPoint(std::min(margin,std::max(0,available-m_zoomHud->GetSize().x)),
        std::max(0,client.y-scroll-margin-m_zoomHud->GetSize().y)));
    m_floatToolbar->Raise();m_zoomHud->Raise();
}

void CanvasPanel::SyncCanvasControls() {
    if (m_floatToolbar) m_floatToolbar->Sync(m_toolStateMachine->GetCurrentTool());
    if (m_zoomHud) m_zoomHud->Sync(m_scale);
    if (m_HandyToolKit) m_HandyToolKit->Refresh(false);
}

void CanvasPanel::ActivateCanvasTool(ToolType tool) {
    FinishPointerGesture();
    if (tool==ToolType::COMPONENT_TOOL && !m_placingName.IsEmpty()) SetCurrentComponent(m_placingName);
    else SetCurrentTool(tool);
    if (tool==ToolType::COMPONENT_TOOL && m_placingName.IsEmpty()) {
        m_mainFrame->mf->ShowComponentLibrary();
        SetStatus("Component: choose from Component Library");
    } else SetFocus();
}

wxPoint CanvasPanel::ClientToCanvas(const wxPoint& screenPos) const{
    return wxPoint(
        static_cast<int>((screenPos.x - m_offset.x) / GetDeviceScale()),
        static_cast<int>((screenPos.y - m_offset.y) / GetDeviceScale())
    );
}

wxPoint CanvasPanel::CanvasToClient(const wxPoint& canvasPos) const
{
    return wxPoint(
        static_cast<int>(canvasPos.x * GetDeviceScale() + m_offset.x),
        static_cast<int>(canvasPos.y * GetDeviceScale() + m_offset.y)
    );
}

void CanvasPanel::RefreshCanvasAfterElementMove()
{
    // wxWidgets merges repeated Refresh calls from one mouse event. This is more
    // reliable than a partial invalidation because Draw() also paints labels and
    // selection effects outside GetBounds().
    Refresh(false);
}

void CanvasPanel::OnPaint(wxPaintEvent&) {
    LayoutScrollbars(false); // 绘制期更新几何，不再次置脏当前画布。
    wxAutoBufferedPaintDC dc(this);
    dc.Clear();

    wxRect updateRect = GetUpdateRegion().GetBox();

    std::unique_ptr<wxGraphicsContext> gc(wxGraphicsRenderer::GetDefaultRenderer()->CreateContext(dc));


    if (gc) {

        const double screenDip = CanvasVisuals::ScreenDip(this, m_scale);
        const bool selecting = m_toolStateMachine->GetCurrentTool() == ToolType::SELECT_TOOL;

        // 应用缩放和偏移（逻辑坐标 -> 设备坐标）
        gc->Scale(GetDeviceScale(), GetDeviceScale());
        gc->Translate(m_offset.x / GetDeviceScale(), m_offset.y / GetDeviceScale());

        // 绘制网格（逻辑坐标，线宽随缩放自适应）
        // T-410：细网格 20 DIP = token `grid`，粗网格 100 DIP = token `gridStrong`（CMP-041）。
        // 只替换取色与叠加粗网格两层，重绘流程 / 双缓冲策略不变（PERF-002/PERF-003）。
        const wxColour gridColor = UiTheme::Palette().grid;
        gc->SetPen(gc->CreatePen(wxGraphicsPenInfo(gridColor).Width(1.0 / GetDeviceScale()))); // 笔宽在逻辑坐标下调整

        wxSize sz = wxSize(m_size.x + 1, m_size.y + 1);
        int maxX = static_cast<int>(sz.x);
        int maxY = static_cast<int>(sz.y);

        for (int x = 0; x < maxX; x += m_grid) {
            gc->StrokeLine(x, 0, x, maxY);
        }
        for (int y = 0; y < maxY; y += m_grid) {
            gc->StrokeLine(0, y, maxX, y);
        }

        // 粗网格叠加层（100 DIP，CMP-041）；共享同一 GC，笔宽公式与原细网格一致
        const wxColour gridStrongColor = UiTheme::Palette().gridStrong;
        gc->SetPen(gc->CreatePen(wxGraphicsPenInfo(gridStrongColor).Width(1.0 / GetDeviceScale())));
        for (int x = 0; x < maxX; x += CanvasDraw::kGridCoarse) {
            gc->StrokeLine(x, 0, x, maxY);
        }
        for (int y = 0; y < maxY; y += CanvasDraw::kGridCoarse) {
            gc->StrokeLine(0, y, maxX, y);
        }

        // 绘制导线（矢量线段）
        // 线宽与结点按屏幕 DIP 补偿；选中线在同一次 Draw 中使用 accent，不重叠铺黑底。
        for (size_t i = 0; i < m_wires.size(); ++i) {
            const bool selected = selecting && std::find(m_selWireIdx.begin(), m_selWireIdx.end(),
                                                         static_cast<int>(i)) != m_selWireIdx.end();
            m_wires[i].Draw(gc.get(), screenDip, selected);
        }
        if (m_toolStateMachine->GetWireState() == WireToolState::WIRE_DRAWING)
            m_previewWire.Draw(gc.get(), screenDip);

        if(!m_tbox.m_shapes.empty()) m_tbox.Draw(gc.get(), this, screenDip);

        // 绘制元素（使用矢量绘制）
        for (size_t i = 0; i < m_elems.size(); ++i) {
            const bool selected = selecting && std::find(m_selElemIdx.begin(), m_selElemIdx.end(),
                                                         static_cast<int>(i)) != m_selElemIdx.end();
            m_elems[i].Draw(gc.get(), this, selected, screenDip);
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

        // 悬停引脚高亮（T-410：原亮绿 wxColour(0,255,0) → token accent，空心圆）
        if (m_hoverInfo.IsOverPin()) {
            gc->SetBrush(*wxTRANSPARENT_BRUSH);
            gc->SetPen(wxPen(UiTheme::Palette().accent, 1.0 / m_scale));
            gc->DrawEllipse(m_hoverInfo.snappedPos.x - 3, m_hoverInfo.snappedPos.y - 3, 6, 6);
        }

        if (m_hoverInfo.IsOverCell()) {
            gc->SetBrush(*wxTRANSPARENT_BRUSH);
            gc->SetPen(wxPen(UiTheme::Palette().accent, 1.0 / m_scale));
            gc->DrawEllipse(m_hoverInfo.cellPos.x - 3, m_hoverInfo.cellPos.y - 3, 6, 6);

            // T-410：原 wxColour(*wxBLACK) → token text1（控制点提示，保持深色可辨）
            gc->SetPen(wxPen(UiTheme::Palette().text1, 1.0 / m_scale));
            gc->SetBrush(wxBrush(UiTheme::Palette().text1));
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

        // 1.6 DIP 蓝描边与 8 个 7 DIP 视觉标记；不再叠加错误的品牌绿光晕。
        if (selecting) {
            for (size_t i = 0; i < m_selElemIdx.size(); i++) {
                const int index = m_selElemIdx[i];
                if (index < 0 || index >= static_cast<int>(m_elems.size())) continue;
                CanvasVisuals::DrawSelection(*gc, m_elems[index].GetBounds(), screenDip, false);
            }
            for (size_t i = 0; i < m_selTxtIdx.size(); i++) {
                const int index = m_selTxtIdx[i];
                if (index < 0 || index >= static_cast<int>(m_textElements.size())) continue;
                CanvasVisuals::DrawSelection(*gc, m_textElements[index].GetBounds(), screenDip);
            }
        }

        // 幽灵最后绘制，避免被元件卡覆盖；只改视觉，不改变吸附或落点逻辑。
        if (m_pointerInside && m_toolStateMachine->GetComponentState() == ComponentToolState::COMPONENT_PREVIEW) {
            wxString label = m_previewElement.GetIdentifier();
            if (label.StartsWith("new_")) label = label.Mid(4);
            CanvasVisuals::DrawGhost(*gc, m_previewElement.GetBounds(), label,
                CanvasVisuals::WorldFont(this, UiTheme::TextSize::P13), screenDip);
        }

        // 绘制选择边框
        // 框选反馈使用同一蓝描边与 sel 透明底。
        if (m_toolStateMachine->GetSelectState() == SelectToolState::RECTANGLE_SELECT) {
            wxRect selRect = m_selectRect;
            gc->SetPen(gc->CreatePen(wxGraphicsPenInfo(UiTheme::Palette().accent).Width(1.6 * screenDip)));
            gc->SetBrush(wxBrush(UiTheme::Palette().sel));
            gc->DrawRectangle(selRect.x, selRect.y, selRect.width, selRect.height);
        }

        // T-410：原 wxColor(128,128,128[,32]) → token text3（同色相最近的中性档）
        if (m_toolStateMachine->GetEraserState() == EraserToolState::RECTANGLE_ERASER) {
            wxRect eraRect = m_eraserRect;
            const wxColour eraserColour = UiTheme::Palette().text3;
            gc->SetPen(wxPen(eraserColour, 2 / m_scale));
            gc->SetBrush(wxBrush(eraserColour));
            gc->DrawRectangle(eraRect.x, eraRect.y, eraRect.width, eraRect.height);
        }

    }
}

std::tuple<int, int> CanvasPanel::HitElementAndPinTest(const wxPoint& canvasPos, bool* isInput, wxPoint* worldPos)
{
    int elem_id = -1;
    int pin_id = -1;
    const int pinRadius = std::max(4, static_cast<int>(std::ceil(6.0 / m_scale)));
    for (size_t i = 0; i < m_elems.size(); ++i) {
        // 元素的边界是画布坐标，直接比较
        if (pin_id < 0 && m_elems[i].GetBounds().Contains(canvasPos)) {
            elem_id = i;
        }
        const auto& elem = m_elems[i];
        // 输入引脚尖端（突出 1 px）
        for (size_t p = 0; p < elem.GetInputPins().size(); ++p) {
            wxPoint tip = elem.GetPos() + wxPoint(elem.GetInputPins()[p].pos.x - 1,
                elem.GetInputPins()[p].pos.y);
            if (abs(canvasPos.x - tip.x) <= pinRadius && abs(canvasPos.y - tip.y) <= pinRadius) {
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
            if (abs(canvasPos.x - tip.x) <= pinRadius && abs(canvasPos.y - tip.y) <= pinRadius) {
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
            if (abs(canvasPos.x - tip.x) <= pinRadius && abs(canvasPos.y - tip.y) <= pinRadius) {
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

bool CanvasPanel::Save() {
    if (!tn || !tn->GetParent() || !tn->GetParent()->GetParent()) {
        return false;
    }
    auto* n = tn->GetParent()->GetParent();
    if (n->type != SigTreeNodeType::Project) {
        return false;
    }
    ProjectNode* pn = static_cast<ProjectNode*>(n);
    wxString cwd = pn->projectPath;
    wxFileName targetDir;
    targetDir.AssignDir(cwd + "/.sigflow/canvas");

    if (!targetDir.DirExists()) {
        if (!targetDir.Mkdir(wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL)) {
            return false;
        }
    }

    // 1. 修正文件名：加上 .json 后缀
    wxString filename = GetNote();
    if (!filename.EndsWith(".json")) {
        filename += ".json";
    }

    wxFileName filepath(targetDir.GetPath(), filename);

    // 2. 构建 JSON 数据结构
    json root;
    root["canvas_name"] = ToUtf8(GetNote());
    root["elements"] = json::array(); // 初始化元素数组
    

    // 3. 遍历 m_elems 并填充数据
    for (const auto& elem : m_elems) {
        json element;
        element["type"] = ToUtf8(elem.type);
        element["id"] = ToUtf8(elem.GetIdentifier());
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
    // 先写临时文件再原子替换，避免中途失败留下截断的画布 JSON。
    // dump() 在字符串不是合法 UTF-8 时会抛 type_error.316；Save() 可能在 UI 事件里被调用，
    // 异常一旦逃出去就是 std::terminate，所以必须在这里兜住并上报失败。
    std::string serialized;
    try {
        serialized = root.dump(4);
    }
    catch (const std::exception& error) {
        MyLog("CanvasPanel::Save: JSON serialization failed: %s\n", error.what());
        return false;
    }
    const wxString temporaryPath = filepath.GetFullPath() + ".tmp";
    wxFile file(temporaryPath, wxFile::write);
    if (!file.IsOpened() ||
        file.Write(serialized.data(), serialized.size()) !=
            static_cast<wxFileOffset>(serialized.size())) {
        file.Close();
        wxRemoveFile(temporaryPath);
        return false;
    }
    file.Close();
    if (!wxRenameFile(temporaryPath, filepath.GetFullPath(), true)) {
        wxRemoveFile(temporaryPath);
        return false;
    }

    ResetModified();
    return true;
}



bool CanvasPanel::Read() {
    // 与 Save() 保持一致的三层判空：换项目/关闭标签页期间 tn 可能已经为空，
    // 旧实现直接 tn->GetParent()->GetParent() 会空指针崩溃。
    if (!tn || !tn->GetParent() || !tn->GetParent()->GetParent()) {
        return false;
    }
    auto* n = tn->GetParent()->GetParent();
    ProjectNode* pn = static_cast<ProjectNode*>(n);
    wxString cwd = pn->projectPath;
    wxString name = GetNote();
    wxFileName filepath(cwd + "/.sigflow/canvas", name + ".json");

    if (!filepath.FileExists()) {
        return false;
    }

    const std::filesystem::path canvasPath(sigflow::platform::Utf8Path(filepath.GetFullPath()));
    std::ifstream file(canvasPath);
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
            idMap[ToUtf8(elem.GetIdentifier())] = &elem;
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
    // 只规范化空闲工具提示，保留连线、选择、放置等业务反馈。
    if (status.StartsWith("Tool: ") || status=="Select Tool" || status=="Text Tool" ||
        status=="Wire Tool" || status=="Eraser Tool" || status.StartsWith("Pan Tool:"))
        status=CanvasControls::ToolName(m_toolStateMachine->GetCurrentTool())+wxString::FromUTF8(" · Ready");
    m_mainFrame->SetStatusText(status, 0);

}

void CanvasPanel::SetCurrentTool(ToolType tool) {
    m_CanvasEventHandler->SetCurrentTool(tool);
    SyncCanvasControls();
}

void CanvasPanel::SetCurrentComponent(const wxString& componentName) {
    m_placingName=componentName;
    m_CanvasEventHandler->SetCurrentComponent(componentName);
    m_toolStateMachine->SetComponentState(ComponentToolState::COMPONENT_PREVIEW);
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
    // 八个标记具有 11 屏幕 DIP 抓取半径；沿用现有移动/选择入口。
    // 引脚优先，避免热区抢走连线操作；不改变模型的形状和边界。
    if (pinIdx<0 && m_toolStateMachine->GetCurrentTool()==ToolType::SELECT_TOOL) {
        const double units=CanvasVisuals::ScreenDip(this,m_scale);
        for (auto i:m_selElemIdx) {
            if (i>=0 && i<int(m_elems.size()) && CanvasVisuals::HitHandle(m_elems[i].GetBounds(),m_hoverInfo.canvasPos,units)) {
                elementIndex=i; textIndex=-1; wireIdx=-1; cellIdx=-1; break;
            }
        }
        if (elementIndex<0) for (auto i:m_selTxtIdx) {
            if (i>=0 && i<int(m_textElements.size()) && CanvasVisuals::HitHandle(m_textElements[i].GetBounds(),m_hoverInfo.canvasPos,units)) {
                textIndex=i; wireIdx=-1; cellIdx=-1; break;
            }
        }
    }

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
    else {
        hover = "Blank Space";
    }

    wxString cursor = wxString::Format("(%d, %d)", m_hoverInfo.canvasPos.x, m_hoverInfo.canvasPos.y);

    // 修复后：
    wxString zoom = wxString::Format("%d%%", int(std::lround(m_scale * 100)));

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
    m_spacePan=false;
    if (HasCapture()) {
        m_toolStateMachine->SetSelectState(SelectToolState::IDLE);
        ClearSelectionRect();
    }
    FinishPointerGesture();
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
        m_offset.x = std::lround(-(event.GetPosition()+m_scrollOrigin.x)*GetDeviceScale());
    }
    else {
        m_offset.y = std::lround(-(event.GetPosition()+m_scrollOrigin.y)*GetDeviceScale());
    }
    Refresh();
}

void CanvasPanel::LayoutScrollbars(bool invalidate){
    const wxSize clientSize = GetClientSize();
    // 用系统度量取滚动条宽度（HiDPI 下写死 24 会偏窄）；取不到再退回 24。
    // 旧实现把度量算进 scrollbarSize 却没用，仍然用常量 24。
    int width = wxSystemSettings::GetMetric(wxSYS_VSCROLL_X);
    if (width <= 0) width = 24;

    const double scale = GetDeviceScale();
    // SetThumbSize 传负数在 GTK 上会触发断言，至少钳到 1。
    const int hThumb = std::max(1, static_cast<int>((clientSize.x - width - 1) / scale));
    const int vThumb = std::max(1, static_cast<int>((clientSize.y - width - 1) / scale));

    bool changed = false;
    // 包含当前视口的虚拟滚动范围，允许 25% 缩小和双向平移而不跳回原点。
    const int left=std::lround(-m_offset.x/scale),top=std::lround(-m_offset.y/scale);
    m_scrollOrigin=wxPoint(std::min(0,left),std::min(0,top));
    const auto apply = [&changed](wxScrollBar* bar, const wxSize& size,
                                  const wxPoint& pos, int thumb, int range, int value) {
        if (bar->GetSize() != size || bar->GetPosition() != pos ||
            bar->GetThumbSize() != thumb || bar->GetRange() != range ||
            bar->GetThumbPosition() != value) {
            bar->SetSize(size);
            bar->SetPosition(pos);
            bar->SetThumbSize(thumb);
            bar->SetRange(range);
            bar->SetThumbPosition(value);
            changed = true;
        }
    };

    // 水平滚动条：底部，宽度要减去垂直滚动条的宽度
    apply(m_hScroll, wxSize(clientSize.x - width, width),
          wxPoint(0, clientSize.y - width), hThumb, std::max(m_size.x,left+hThumb)-m_scrollOrigin.x,
          left-m_scrollOrigin.x);

    // 垂直滚动条：右侧，高度要减去水平滚动条的高度
    apply(m_vScroll, wxSize(width, clientSize.y),
          wxPoint(clientSize.x - width, 0), vThumb, std::max(m_size.y,top+vThumb)-m_scrollOrigin.y,
          top-m_scrollOrigin.y);

    // 只在几何真的变化时重绘。
    //
    // 旧实现无条件 Refresh()，而 OnPaint() 第一件事就是调用 LayoutScrollbars()，
    // 于是"每次绘制都把窗口重新置脏" → 自激重绘循环，长期占满一个核
    // （与 ProjectTreePanel 的文件监视自激是同一类问题）。
    // 现在 OnPaint 里几何已是最新 → changed 为 false → 不再自激；
    // 而缩放/尺寸变化时会 changed=true → 触发一次重绘，行为不变。
    if (changed && invalidate) {
        Refresh(false);
    }
}

void CanvasPanel::SetoffSet(wxPoint offset) {
    // 平移是视图变换，不夹取到模型范围；否则缩小会破坏光标缩放锚点。
    m_offset = offset;
    for (auto& text:m_textElements) text.SetPosition(text.GetPosition());
    Refresh();
}

wxPoint CanvasPanel::LogicToDevice(const wxPoint& logicPoint) const
{
    return wxPoint(
        static_cast<int>(logicPoint.x * GetDeviceScale() + m_offset.x),
        static_cast<int>(logicPoint.y * GetDeviceScale() + m_offset.y)
    );
}

wxPoint CanvasPanel::DeviceToLogic(const wxPoint& devicePoint) const
{
    return wxPoint(
        static_cast<int>((devicePoint.x - m_offset.x) / GetDeviceScale()),
        static_cast<int>((devicePoint.y - m_offset.y) / GetDeviceScale())
    );
}

std::pair<wxPoint, wxPoint> CanvasPanel::ValidSetOffRange() {
    // (0, 0)的逻辑坐标对应的设备坐标为 m_offset，因此m_offset的最大值为(0,0)
    wxPoint maxOffset(0, 0);
    // 计算逻辑坐标 (m_size.x, m_size.y) 对应的设备坐标的最大值为 (GetClientSize().x- m_hScroll->GetSize().y - 1, GetClientSize().y- m_hScroll->GetSize().y - 1)，此时的 m_offset 即为最小值
    wxPoint minOffset(
        GetClientSize().x - static_cast<int>(m_size.x * GetDeviceScale()) - m_hScroll->GetSize().y - 1,
        GetClientSize().y - static_cast<int>(m_size.y * GetDeviceScale()) - m_hScroll->GetSize().y - 1
    );

    return std::make_pair(minOffset, maxOffset);
}

std::pair<float, float> CanvasPanel::ValidScaleRange() {
    return {0.25f,4.0f};
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
    if (index < 0 || index >= static_cast<int>(m_wires.size())) return;
    for (auto& cp : m_wires[index].pts) {
        cp.pos += offset;
    }
    m_wires[index].GenerateCells();
}

void CanvasPanel::WirePtsSetPos(int wireIndex, int controlPointIndex, const wxPoint& pos) {
    if (wireIndex < 0 || wireIndex >= static_cast<int>(m_wires.size())) return;
    if (controlPointIndex < 0 || controlPointIndex >= static_cast<int>(m_wires[wireIndex].pts.size())) return;
    m_wires[wireIndex].pts[controlPointIndex].pos = pos;
    m_wires[wireIndex].GenerateCells();
    Refresh();
    //触发改变
    SetModified(1);
}


void CanvasPanel::UpdateSelection(std::vector<int> m_elemIdx, std::vector<int> m_textIdx, std::vector<int> m_wireIdx ) {
    m_selTxtIdx = m_textIdx;
    m_selElemIdx = m_elemIdx;
    m_selWireIdx = m_wireIdx;
    Refresh();
}

void CanvasPanel::AddWire(const Wire& wire) {
    m_wires.push_back(wire); 
    m_wires.back().GenerateCells();
    auto& w = m_wires.back();

    auto resolveEndpoint = [this](Endpoint endpoint, bool left, int& elemId,
                                  int& pinId, bool& isInput) {
        std::unordered_set<int> visited;
        while (endpoint.elemIdx == -1) {
            if (endpoint.wireIdx < 0 ||
                endpoint.wireIdx >= static_cast<int>(m_wires.size()) ||
                !visited.insert(endpoint.wireIdx).second) {
                return false;
            }
            endpoint = left ? m_wires[endpoint.wireIdx].Left
                            : m_wires[endpoint.wireIdx].Right;
        }
        elemId = endpoint.elemIdx;
        pinId = endpoint.PinIdx;
        isInput = endpoint.isInput;
        return elemId >= 0 && elemId < static_cast<int>(m_elems.size()) && pinId >= 0;
    };

    int from_id = -1;
    int from_pin = -1;
    bool is_input = false;
    int to_id = -1;
    int to_pin = -1;
    bool ignoredToIsInput = false;
    if (!resolveEndpoint(w.Left, true, from_id, from_pin, is_input) ||
        !resolveEndpoint(w.Right, false, to_id, to_pin, ignoredToIsInput)) {
        m_wires.pop_back();
        wxLogWarning("Ignoring wire with an invalid or cyclic endpoint reference.");
        return;
    }

    SecondElement& from = m_elems[from_id];
    SecondElement& to = m_elems[to_id];
    if (!from.self || !to.self ||
        (is_input && (from_pin >= static_cast<int>(from.self->in_ports.size()) ||
                      to_pin >= static_cast<int>(to.self->out_ports.size()))) ||
        (!is_input && (from_pin >= static_cast<int>(from.self->out_ports.size()) ||
                       to_pin >= static_cast<int>(to.self->in_ports.size())))) {
        m_wires.pop_back();
        wxLogWarning("Ignoring wire with an invalid element pin reference.");
        return;
    }

    SignalNode* sn = sftree->AddNewWire(tn);
    wxCommandEvent evt2(EVT_SIGFLOWNODE_CHANGED);
    evt2.SetClientData(sn);
    m_parent->GetEventHandler()->ProcessEvent(evt2);
    w.SetSelf(sn);

    if (is_input) {

        from.self->in_ports[from_pin].conn = sn->identifier;
        wxCommandEvent evt0(EVT_SIGFLOWNODE_CHANGED);
        evt0.SetClientData(from.self);
        m_parent->GetEventHandler()->ProcessEvent(evt0);

        to.self->out_ports[to_pin].conn = sn->identifier;
        wxCommandEvent evt(EVT_SIGFLOWNODE_CHANGED);
        evt.SetClientData(to.self);
        m_parent->GetEventHandler()->ProcessEvent(evt);
    }
    else {
        from.self->out_ports[from_pin].conn = sn->identifier;
        wxCommandEvent evt0(EVT_SIGFLOWNODE_CHANGED);
        evt0.SetClientData(from.self);
        m_parent->GetEventHandler()->ProcessEvent(evt0);

        to.self->in_ports[to_pin].conn = sn->identifier;
        wxCommandEvent evt(EVT_SIGFLOWNODE_CHANGED);
        evt.SetClientData(to.self);
        m_parent->GetEventHandler()->ProcessEvent(evt);
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
            /*
            std::vector<ControlPoint> clean;
            for (auto& p : pts)
                if (clean.empty() || clean.back().pos != p.pos)
                    clean.push_back(p);

            if (static_cast<int>(clean.size()) >= 2) {
                Wire w(std::move(clean));
                w.SetSelf(sn);
                w.m_canvas = this;
                m_wires.push_back(std::move(w));
            }*/
            auto clean = cleanPath(pts);
            if (static_cast<int>(clean.size()) >= 2)
                forwardPaths.push_back(std::move(clean));
        }

        addWiresForPaths(addWiresForPaths, forwardPaths);
    }

    for (auto& w : m_wires) w.GenerateCells();
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

    GateInstNode gn("new_"+SigFlowTree::ToString(type), type);
    sftree->AddChild(tn, &gn);
    Refresh();
}

void CanvasPanel::AddModuleInstNode(wxString def, wxPoint pos) {
    extern std::vector<SecondElement> g_elements;
    ModuleInstNode mn("new_" + ToUtf8(def), ToUtf8(def));
    sftree->AddChild(tn, &mn);
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
    RefreshCanvasAfterElementMove();
    //触发改变
    SetModified(1);
}

void CanvasPanel::AddSecondNode(wxString type, wxPoint pos) {
    GateType gt = SigFlowTree::GateTypeFromString(ToUtf8(type));
    if (gt != GateType::Unknown) AddGateNode(gt, pos);
    else AddModuleInstNode(type, pos);
}

void CanvasPanel::SetPreview(wxString type) {
    GateType gt = SigFlowTree::GateTypeFromString(ToUtf8(type));
    if (gt != GateType::Unknown) {
        extern std::vector<SecondElement> g_elements;

        m_previewNode = std::make_unique<GateInstNode>("new_" + SigFlowTree::ToString(gt), gt);
        SecondElement se = SecondElement(m_previewNode.get(), g_elements);
        se.SetPos(wxPoint(0, 0));
        m_previewElement = se;
        RefreshCanvasAfterElementMove();
    }
    else {
        extern std::vector<SecondElement> g_elements;
        auto previewNode = std::make_unique<ModuleInstNode>("new_" + type.ToStdString(), type.ToStdString());
        ModuleInstNode* mn = previewNode.get();
        sftree->LinkSingleInstWithDef(mn);
        if (mn->Definition) {
            m_previewNode = std::move(previewNode);
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
    RefreshCanvasAfterElementMove();
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
