#pragma once

#include "ToolStateMachine.h"
#include "ui/UiFocusRing.h"
#include "ui/UiTheme.h"
#include <wx/control.h>
#include <wx/dcbuffer.h>
#include <wx/graphics.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/access.h>
#include <array>
#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>

// T-420: vector icons and ordinary frosted surfaces; the Synthesize glass recipe is not used.
namespace CanvasControls {
enum class Icon { Select, Text, Component, Wire, Pan, Eraser, Minus, Plus, Fit };
inline constexpr std::array<ToolType,6> Tools={ToolType::SELECT_TOOL,ToolType::TEXT_TOOL,
    ToolType::COMPONENT_TOOL,ToolType::WIRE_TOOL,ToolType::DRAG_TOOL,ToolType::ERASER_TOOL};
inline wxString ToolName(ToolType tool) {
    switch(tool) {
        case ToolType::SELECT_TOOL:return "Select";
        case ToolType::TEXT_TOOL:return "Text";
        case ToolType::COMPONENT_TOOL:return "Component";
        case ToolType::WIRE_TOOL:return "Wire";
        case ToolType::DRAG_TOOL:return "Pan";
        case ToolType::ERASER_TOOL:return "Eraser";
        default:return "Drawing";
    }
}
inline Icon ToolIcon(ToolType tool) {
    for(size_t i=0;i<Tools.size();++i) if(Tools[i]==tool) return static_cast<Icon>(i);
    return Icon::Select;
}
inline void DrawCapsule(wxGraphicsContext& gc,const wxRect& r,double dip) {
    const auto& c=UiTheme::Palette();
    gc.SetPen(gc.CreatePen(wxGraphicsPenInfo(c.stroke).Width(dip)));
    gc.SetBrush(wxBrush(c.surfaceOnCanvas));
    const double radius=r.height>40*dip?UiTheme::Radius::MD*dip:r.height/2.0;
    gc.DrawRoundedRectangle(r.x+dip/2,r.y+dip/2,r.width-dip,r.height-dip,radius);
}
inline void DrawButton(wxGraphicsContext& gc,const wxRect& r,double dip,bool selected,bool hover,bool focus,bool enabled=true,bool keyboardFocus=false) {
    const auto& c=UiTheme::Palette();
    if(selected) gc.SetPen(gc.CreatePen(wxGraphicsPenInfo(c.accent).Width(dip)));
    else gc.SetPen(*wxTRANSPARENT_PEN);
    gc.SetBrush(wxBrush(selected?c.selOnPanel:(hover&&enabled?c.hoverOnPanel:c.surfaceOnCanvas)));
    gc.DrawRoundedRectangle(r.x+dip,r.y+dip,r.width-2*dip,r.height-2*dip,6*dip);
    // A11Y-001：键盘聚焦画 accent 焦点环；DEC-14：点击后保留焦点只给灰色提示。
    if(focus&&keyboardFocus) {
        UiFocusRing::Draw(gc,r,dip,6*dip);
    } else if(focus) {
        gc.SetBrush(*wxTRANSPARENT_BRUSH);
        gc.SetPen(gc.CreatePen(wxGraphicsPenInfo(c.text3).Width(dip)));
        gc.StrokeLine(r.x+6*dip,r.GetBottom()-4*dip,r.GetRight()-6*dip,r.GetBottom()-4*dip);
    }
}
inline void DrawIcon(wxGraphicsContext& gc,Icon icon,const wxRect& r,double dip,const wxColour& colour) {
    const auto saved=gc.GetTransform();
    const double size=16*dip;
    gc.Translate(r.x+(r.width-size)/2,r.y+(r.height-size)/2);gc.Scale(size/24,size/24);
    gc.SetPen(gc.CreatePen(wxGraphicsPenInfo(colour).Width(1.7).Join(wxJOIN_ROUND).Cap(wxCAP_ROUND)));
    gc.SetBrush(*wxTRANSPARENT_BRUSH);
    auto line=[&](double a,double b,double c,double d){gc.StrokeLine(a,b,c,d);};
    auto polygon=[&](std::initializer_list<wxPoint2DDouble> points) {
        auto p=gc.CreatePath();bool first=true;
        for(const auto& point:points) {if(first) p.MoveToPoint(point);else p.AddLineToPoint(point);first=false;}
        p.CloseSubpath();gc.StrokePath(p);
    };
    switch(icon) {
        case Icon::Select:polygon({{5,3},{19,11},{13,12.5},{10,19}});break;
        case Icon::Text:line(5,6,19,6);line(12,6,12,19);line(9,19,15,19);break;
        case Icon::Component:gc.DrawRoundedRectangle(4,4,16,16,2);gc.DrawRectangle(9,9,6,6);break;
        case Icon::Wire:line(5,16,5,6);line(5,6,15,6);gc.DrawEllipse(15.6,3.6,4.8,4.8);gc.DrawEllipse(2.6,15.6,4.8,4.8);break;
        case Icon::Pan:
            line(12,3,12,21);line(3,12,21,12);
            line(12,3,9.5,5.5);line(12,3,14.5,5.5);line(12,21,9.5,18.5);line(12,21,14.5,18.5);
            line(3,12,5.5,9.5);line(3,12,5.5,14.5);line(21,12,18.5,9.5);line(21,12,18.5,14.5);break;
        case Icon::Eraser:polygon({{4,16},{12,8},{17,13},{13,17},{6,17}});line(11,20,20,20);break;
        case Icon::Minus:line(5,12,19,12);break;
        case Icon::Plus:line(5,12,19,12);line(12,5,12,19);break;
        case Icon::Fit:
            line(4,9,4,4);line(4,4,9,4);line(20,9,20,4);line(20,4,15,4);
            line(4,15,4,20);line(4,20,9,20);line(20,15,20,20);line(20,20,15,20);break;
    }
    gc.SetTransform(saved);
}

class ActionButton final:public wxControl {
public:
    ActionButton(wxWindow* parent,Icon icon,const wxString& name,std::function<void()> action)
        :wxControl(parent,wxID_ANY,wxDefaultPosition,wxDefaultSize,wxBORDER_NONE),m_icon(icon),m_action(std::move(action)) {
        SetName(name);SetToolTip(name);SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetMinSize(FromDIP(wxSize(30,28)));SetSize(GetMinSize());
#if wxUSE_ACCESSIBILITY
        SetAccessible(new Accessible(this));
#endif
        Bind(wxEVT_PAINT,[this](wxPaintEvent&) {
            wxAutoBufferedPaintDC dc(this);dc.SetBackground(wxBrush(UiTheme::Palette().surfaceOnCanvas));dc.Clear();
            std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));if(!gc)return;
            const double dip=FromDIP(100)/100.0;
            DrawButton(*gc,GetClientRect(),dip,m_selected,m_hover,HasFocus(),IsEnabled(),
                       m_focusTracker.KeyboardFocused());
            const auto& c=UiTheme::Palette();DrawIcon(*gc,m_icon,GetClientRect(),dip,IsEnabled()?(m_selected?c.accent:c.text2):c.text3);
        });
        Bind(wxEVT_ENTER_WINDOW,[this](wxMouseEvent&){UiFocusTrace::NotePointer();m_hover=true;Refresh(false);});
        Bind(wxEVT_LEAVE_WINDOW,[this](wxMouseEvent&){UiFocusTrace::NotePointer();m_hover=false;Refresh(false);});
        Bind(wxEVT_LEFT_DOWN,[this](wxMouseEvent&){UiFocusTrace::NotePointer();if(IsEnabled()){SetFocus();m_down=true;CaptureMouse();Refresh(false);}});
        Bind(wxEVT_LEFT_UP,[this](wxMouseEvent& e){const bool run=m_down&&GetClientRect().Contains(e.GetPosition());m_down=false;if(HasCapture())ReleaseMouse();if(run&&IsEnabled())m_action();Refresh(false);});
        Bind(wxEVT_MOUSE_CAPTURE_LOST,[this](wxMouseCaptureLostEvent&){m_down=false;Refresh(false);});
        Bind(wxEVT_SET_FOCUS,[this](wxFocusEvent& e){m_focusTracker.OnFocusGained();Refresh(false);e.Skip();});
        Bind(wxEVT_KILL_FOCUS,[this](wxFocusEvent& e){
            m_keyDown=false;m_focusTracker.OnFocusLost();
            if(HasCapture())ReleaseMouse();  // 不得留下「点住后按 Tab」的捕获陷阱（A11Y-002）
            Refresh(false);e.Skip();
        });
        Bind(wxEVT_KEY_DOWN,[this](wxKeyEvent& e){UiFocusTrace::NoteKeyboard();if(e.GetKeyCode()==WXK_SPACE||e.GetKeyCode()==WXK_RETURN){if(IsEnabled())m_keyDown=true;}else e.Skip();});
        Bind(wxEVT_KEY_UP,[this](wxKeyEvent& e){UiFocusTrace::NoteKeyboard();if(e.GetKeyCode()==WXK_SPACE||e.GetKeyCode()==WXK_RETURN){const bool run=m_keyDown;m_keyDown=false;if(run&&IsEnabled())m_action();}else e.Skip();});
    }
    bool AcceptsFocus()const override{return IsEnabled()&&IsShown();}
    bool AcceptsFocusFromKeyboard()const override{return AcceptsFocus();}
    void SetPressed(bool value){if(m_selected!=value){m_selected=value;Refresh(false);}}
    bool IsPressed()const{return m_selected;}
private:
#if wxUSE_ACCESSIBILITY
    class Accessible final:public wxAccessible {
    public:
        explicit Accessible(ActionButton* b):wxAccessible(b),button(b){}
        wxAccStatus GetName(int id,wxString* name)override{if(id!=wxACC_SELF)return wxACC_NOT_IMPLEMENTED;*name=button->GetName();return wxACC_OK;}
        wxAccStatus GetRole(int id,wxAccRole* role)override{if(id!=wxACC_SELF)return wxACC_NOT_IMPLEMENTED;*role=wxROLE_SYSTEM_PUSHBUTTON;return wxACC_OK;}
        wxAccStatus GetState(int id,long* state)override{if(id!=wxACC_SELF)return wxACC_NOT_IMPLEMENTED;*state=wxACC_STATE_SYSTEM_FOCUSABLE;if(button->HasFocus())*state|=wxACC_STATE_SYSTEM_FOCUSED;if(button->IsPressed())*state|=wxACC_STATE_SYSTEM_PRESSED;if(!button->IsEnabled())*state|=wxACC_STATE_SYSTEM_UNAVAILABLE;return wxACC_OK;}
    private:ActionButton* button;
    };
#endif
    Icon m_icon;std::function<void()> m_action;
    bool m_selected=false,m_hover=false,m_down=false,m_keyDown=false;
    UiFocusRing::Tracker m_focusTracker;  ///< T-620：键盘焦点 vs 点击后焦点保留
};

class Capsule:public wxPanel {
public:
    explicit Capsule(wxWindow* parent,const wxString& name):wxPanel(parent,wxID_ANY,wxDefaultPosition,wxDefaultSize,wxBORDER_NONE|wxTAB_TRAVERSAL) {
        SetName(name);SetBackgroundStyle(wxBG_STYLE_PAINT);
        Bind(wxEVT_PAINT,[this](wxPaintEvent&) {
            wxAutoBufferedPaintDC dc(this);dc.SetBackground(wxBrush(UiTheme::Palette().canvas));dc.Clear();
            std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));if(gc)DrawCapsule(*gc,GetClientRect(),FromDIP(100)/100.0);
        });
        // Scrolling on chrome must not zoom or place content on the canvas beneath it.
        Bind(wxEVT_MOUSEWHEEL,[](wxMouseEvent&){});
        theme=UiTheme::Subscribe([this]{Refresh(false);for(auto* child:GetChildren())child->Refresh(false);});
    }
private:UiTheme::Subscription theme;
};
class FloatToolbar final:public Capsule {
public:
    FloatToolbar(wxWindow* parent,std::function<void(ToolType)> activate):Capsule(parent,"CanvasFloatToolbar") {
        grid=new wxGridSizer(0,6,FromDIP(3),FromDIP(3));
        for(size_t i=0;i<Tools.size();++i){auto tool=Tools[i];buttons[i]=new ActionButton(this,ToolIcon(tool),ToolName(tool),[activate,tool]{activate(tool);});grid->Add(buttons[i]);}
        auto* outer=new wxBoxSizer(wxHORIZONTAL);outer->Add(grid,0,wxALL,FromDIP(5));SetSizerAndFit(outer);
    }
    void FitWidth(int width){const int cols=std::clamp((width-FromDIP(10)+FromDIP(3))/std::max(1,buttons[0]->GetSize().x+FromDIP(3)),1,6);if(cols!=grid->GetCols())grid->SetCols(cols);const auto size=GetSizer()->CalcMin();if(GetSize()!=size){SetMinSize(size);SetSize(size);Layout();}}
    void Sync(ToolType tool){for(size_t i=0;i<Tools.size();++i)buttons[i]->SetPressed(Tools[i]==tool);}
    ActionButton* Button(size_t i)const{return buttons.at(i);}
private:std::array<ActionButton*,6> buttons{};wxGridSizer* grid;
};
class ZoomHud final:public Capsule {
public:
    ZoomHud(wxWindow* parent,std::function<void(int)> step,std::function<void()> fit):Capsule(parent,"ZoomHud") {
        minus=new ActionButton(this,Icon::Minus,"Zoom out",[step]{step(-1);});
        plus=new ActionButton(this,Icon::Plus,"Zoom in",[step]{step(1);});
        reset=new ActionButton(this,Icon::Fit,"Fit: 100% and reset pan",std::move(fit));
        value=new wxStaticText(this,wxID_ANY,"100%",wxDefaultPosition,FromDIP(wxSize(44,-1)),wxALIGN_CENTER|wxST_NO_AUTORESIZE);
        value->SetName("ZoomPercent");value->SetFont(UiTheme::MonoFont(value,UiTheme::TextSize::P12));
        value->SetBackgroundColour(UiTheme::Palette().surfaceOnCanvas);value->SetForegroundColour(UiTheme::Palette().text2);
        row=new wxBoxSizer(wxHORIZONTAL);row->Add(minus);row->Add(value,0,wxALIGN_CENTER_VERTICAL|wxLEFT|wxRIGHT,FromDIP(4));row->Add(plus);row->AddSpacer(FromDIP(8));row->Add(reset);
        auto* outer=new wxBoxSizer(wxVERTICAL);outer->Add(row,0,wxALL,FromDIP(5));SetSizerAndFit(outer);fullWidth=GetSize().x;
    }
    void FitWidth(int width){const bool next=width<fullWidth;if(next!=compact){if(next){row->Detach(value);GetSizer()->Add(value,0,wxALIGN_CENTER|wxBOTTOM,FromDIP(5));}else{GetSizer()->Detach(value);row->Insert(1,value,0,wxALIGN_CENTER_VERTICAL|wxLEFT|wxRIGHT,FromDIP(4));}compact=next;const auto size=GetSizer()->CalcMin();SetMinSize(size);SetSize(size);Layout();}}
    void Sync(float zoom){value->SetLabel(wxString::Format("%d%%",int(std::lround(zoom*100))));minus->Enable(zoom>0.25001f);plus->Enable(zoom<3.99999f);}
    ActionButton* Minus()const{return minus;}ActionButton* Plus()const{return plus;}ActionButton* Fit()const{return reset;}
    wxString Percent()const{return value->GetLabel();}
private:ActionButton *minus,*plus,*reset;wxStaticText* value;wxBoxSizer* row;int fullWidth;bool compact=false;
};
} // namespace CanvasControls
