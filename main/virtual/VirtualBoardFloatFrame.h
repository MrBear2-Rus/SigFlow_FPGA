#pragma once
#include <wx/frame.h>
#include <functional>
class wxBookCtrlBase;class wxButton;class wxPanel;class wxStaticText;
namespace sigflow::virtual_lab {
class VirtualBoardPanel;
// Independent floating host for the live virtual workbench (SPEC §4.4 and
// planning book §2.1). Popping out and returning only reparent the view:
// the shared VirtualSessionController and its LiveTraceSource keep running
// without any destroyed callbacks or lost session state.
class VirtualBoardFloatFrame:public wxFrame {
public:
    using ReturnHandler=std::function<void()>;
    // Detaches the live panel from its notebook host, shows a placeholder page
    // at the original index and raises the 880x620 DIP frame on the display
    // under the mouse. Never destroys the panel or the controller.
    static VirtualBoardFloatFrame* PopOut(wxBookCtrlBase* host,VirtualBoardPanel* board,const wxString& pageTitle);
    void ReturnToMain();
    VirtualBoardPanel* Board()const{return board_;}
    bool IsReturning()const{return returning_;}
    void SetReturnHandler(ReturnHandler handler){return_handler_=std::move(handler);}
    void SetPlaceholderText(const wxString& message);
private:
    VirtualBoardFloatFrame(wxBookCtrlBase* host,VirtualBoardPanel* board,int pageIndex,const wxString& pageTitle);
    void OnClose(wxCloseEvent&);
    class Placeholder;
    wxBookCtrlBase* host_=nullptr;VirtualBoardPanel* board_=nullptr;
    Placeholder* placeholder_=nullptr;int index_=0;bool returning_=false;
    ReturnHandler return_handler_;
    wxPanel* header_=nullptr;wxStaticText* title_=nullptr;wxButton* return_=nullptr;
};
}
