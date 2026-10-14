#include "HandyToolKit.h"
#include "ui/CanvasControls.h"

wxBEGIN_EVENT_TABLE(HandyToolKit, wxPopupWindow)
EVT_PAINT(HandyToolKit::OnPaint)
EVT_MOTION(HandyToolKit::OnMouseMove)
EVT_RIGHT_UP(HandyToolKit::OnRightUp)
EVT_KILL_FOCUS(HandyToolKit::OnKillFocus)
wxEND_EVENT_TABLE()

HandyToolKit::HandyToolKit(CanvasPanel* parent,CanvasEventHandler* ce)
    :wxPopupWindow(parent,wxBORDER_NONE),m_canvas(parent),m_selectedTool(-1),
     m_hoveredTool(-1),m_CanvasEventHandler(ce)
{
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    m_toolSize=FromDIP(30);CreateTools(m_toolSize);
    SetExtraStyle(GetExtraStyle()|wxWS_EX_TRANSIENT);
}

void HandyToolKit::CreateTools(int size)
{
    m_tools.clear();
    const ToolType order[]={ToolType::DRAG_TOOL,ToolType::SELECT_TOOL,ToolType::ERASER_TOOL,ToolType::TEXT_TOOL,ToolType::WIRE_TOOL};
    const int pad=FromDIP(5),gap=FromDIP(3),height=FromDIP(28);
    for(size_t i=0;i<5;++i) {
        const auto tool=order[i];
        m_tools.push_back({CanvasControls::ToolName(tool),tool,
            wxRect(pad+int(i)*(size+gap),pad,size,height),
            [this,tool]{m_canvas->SetCurrentTool(tool);}});
    }
    SetSize(wxSize(2*pad+5*size+4*gap,2*pad+height));
}

void HandyToolKit::OnPaint(wxPaintEvent&)
{
    wxAutoBufferedPaintDC dc(this);dc.SetBackground(wxBrush(UiTheme::Palette().canvas));dc.Clear();
    std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));if(!gc)return;
    const double dip=FromDIP(100)/100.0;
    CanvasControls::DrawCapsule(*gc,GetClientRect(),dip);
    const auto current=m_canvas->GetToolStateMachine()->GetCurrentTool();
    for(size_t i=0;i<m_tools.size();++i) {
        const auto& tool=m_tools[i];const bool selected=current==tool.type;
        CanvasControls::DrawButton(*gc,tool.rect,dip,selected,int(i)==m_hoveredTool,false);
        CanvasControls::DrawIcon(*gc,CanvasControls::ToolIcon(tool.type),tool.rect,dip,
            selected?UiTheme::Palette().accent:UiTheme::Palette().text2);
    }
}

void HandyToolKit::OnMouseMove(wxMouseEvent& event)
{
    const int old=m_hoveredTool;m_hoveredTool=-1;
    for(size_t i=0;i<m_tools.size();++i) if(m_tools[i].rect.Contains(event.GetPosition())) {
        m_hoveredTool=int(i);m_canvas->SetStatus("Tool: "+m_tools[i].name);break;
    }
    if(old!=m_hoveredTool)Refresh(false);
    event.Skip();
}

void HandyToolKit::OnRightUp(wxMouseEvent& event)
{
    for(size_t i=0;i<m_tools.size();++i) if(m_tools[i].rect.Contains(event.GetPosition())) {
        m_selectedTool=int(i);m_tools[i].action();PassFocusToCanvas();break;
    }
    Hide();
}

void HandyToolKit::OnKillFocus(wxFocusEvent& event)
{
    auto* focused=wxWindow::FindFocus();if(focused&&focused!=GetParent())Hide();event.Skip();
}
void HandyToolKit::PassFocusToCanvas(){m_canvas->SetFocus();}
