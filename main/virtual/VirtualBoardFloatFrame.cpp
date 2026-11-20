#include "VirtualBoardFloatFrame.h"
#include "VirtualBoardPanel.h"
#include "ui/UiTheme.h"
#include <wx/bookctrl.h>
#include <wx/button.h>
#include <wx/display.h>
#include <wx/mousestate.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
namespace sigflow::virtual_lab {
// Placeholder page shown in the main notebook while the workbench is popped
// out: spec text plus a single "return to main window" action.
class VirtualBoardFloatFrame::Placeholder:public wxPanel {
public:
    Placeholder(wxWindow* parent,VirtualBoardFloatFrame* frame):wxPanel(parent),frame_(frame){
        auto* layout=new wxBoxSizer(wxVERTICAL);
        message_=new wxStaticText(this,wxID_ANY,wxString::FromUTF8("已拖出为独立窗口 —— 可拖到另一块显示器"));
        auto* button=new wxButton(this,wxID_ANY,wxString::FromUTF8("回归主窗口"));
        layout->AddStretchSpacer();layout->Add(message_,0,wxALIGN_CENTER_HORIZONTAL);layout->Add(button,0,wxALIGN_CENTER_HORIZONTAL|wxALL,FromDIP(10));layout->AddStretchSpacer();
        button->Bind(wxEVT_BUTTON,[this](wxCommandEvent&){if(frame_)frame_->ReturnToMain();});
        SetSizer(layout);ApplyTheme();
    }
    void ApplyTheme(){const auto& p=UiTheme::Palette();SetBackgroundColour(p.bgPanel);message_->SetForegroundColour(p.text2);
        message_->SetFont(UiTheme::UiFont(this,UiTheme::TextSize::P14));Refresh();}
    void SetPlaceholderText(const wxString& text){message_->SetLabel(text);}
private:
    VirtualBoardFloatFrame* frame_=nullptr;wxStaticText* message_=nullptr;
};
VirtualBoardFloatFrame* VirtualBoardFloatFrame::PopOut(wxBookCtrlBase* host,VirtualBoardPanel* board,const wxString& pageTitle){
    if(!host || !board)return nullptr;
    auto index=host->FindPage(board);
    if(index==wxNOT_FOUND)index=host->GetPageCount()?host->GetPageCount()-1:0;
    return new VirtualBoardFloatFrame(host,board,int(index),pageTitle);
}
VirtualBoardFloatFrame::VirtualBoardFloatFrame(wxBookCtrlBase* host,VirtualBoardPanel* board,int pageIndex,const wxString& pageTitle)
    :wxFrame(nullptr,wxID_ANY,wxString::FromUTF8("SigFlow · 虚拟实验箱（独立窗口）")),host_(host),board_(board),index_(pageIndex){
    // Detach the live view first: the notebook keeps its page slot for the
    // placeholder, and the panel object simply changes its wx parent.
    if(auto current=host_->FindPage(board);current!=wxNOT_FOUND)host_->RemovePage(current);
    placeholder_=new Placeholder(host_,this);
    host_->InsertPage(index_,placeholder_,wxString::FromUTF8("虚拟实验箱（已拖出）"));
    // 34 DIP custom header strip: title plus the return button (SPEC §4.4).
    header_=new wxPanel(this);auto* head=new wxBoxSizer(wxHORIZONTAL);
    title_=new wxStaticText(header_,wxID_ANY,pageTitle+"（独立窗口）");
    title_->SetMinSize(wxSize(0,FromDIP(34)));return_=new wxButton(header_,wxID_ANY,wxString::FromUTF8("回到主窗口"));
    head->Add(title_,1,wxEXPAND|wxLEFT,FromDIP(10));head->Add(return_,0,wxALIGN_CENTER_VERTICAL|wxALL,FromDIP(4));
    header_->SetSizer(head);
    auto* layout=new wxBoxSizer(wxVERTICAL);layout->Add(header_,0,wxEXPAND);
    // Detach from any previous floating layout so a returning panel can be
    // re-hosted; reparenting alone does not clear the containing sizer.
    if(auto* previous=board_->GetContainingSizer())previous->Detach(board_);
    board_->Reparent(this);layout->Add(board_,1,wxEXPAND);SetSizer(layout);
    return_->Bind(wxEVT_BUTTON,[this](wxCommandEvent&){ReturnToMain();});
    Bind(wxEVT_CLOSE_WINDOW,[this](wxCloseEvent&){ReturnToMain();});
    // 880x620 DIP initial size on the display under the mouse.
    auto size=FromDIP(wxSize(880,620));
    auto index=wxDisplay::GetFromPoint(wxGetMousePosition());
    if(index<0)index=wxDisplay::GetFromWindow(host_);if(index<0)index=0;
    wxDisplay display(index);auto area=display.GetClientArea();
    if(size.x>area.width)size.x=area.width;if(size.y>area.height)size.y=area.height;
    SetClientSize(size);
    auto position=area.GetTopLeft()+wxPoint((area.width-size.x)/2,(area.height-size.y)/3);
    SetPosition(position);
    const auto& p=UiTheme::Palette();header_->SetBackgroundColour(p.bgPanel);title_->SetForegroundColour(p.text1);
    title_->SetFont(UiTheme::UiFont(this,UiTheme::TextSize::P14));
    board_->Layout();Layout();
    Show();Raise();
}
void VirtualBoardFloatFrame::SetPlaceholderText(const wxString& message){
    if(placeholder_){placeholder_->SetPlaceholderText(message);placeholder_->ApplyTheme();}
}
void VirtualBoardFloatFrame::ReturnToMain(){
    if(returning_)return;returning_=true;
    if(return_handler_)return_handler_();
    if(board_){
        if(auto* sizer=board_->GetContainingSizer())sizer->Detach(board_);
        board_->Reparent(host_);
        if(auto current=host_->FindPage(placeholder_);current!=wxNOT_FOUND){
            host_->RemovePage(current);
            placeholder_->Destroy();placeholder_=nullptr;
        }
        host_->InsertPage(index_,board_,wxString::FromUTF8("虚拟实验箱"));
        host_->SetSelection(index_);
    }
    Destroy();
}
}
