#pragma once
#include "VirtualSessionController.h"
#include "ui/UiTheme.h"
#include <wx/panel.h>
#include <wx/dialog.h>
#include <wx/weakref.h>
#include <filesystem>
#include <vector>
class wxStaticText;class wxButton;class wxTextCtrl;class wxListCtrl;class wxSpinCtrl;class wxChoice;class wxSizer;class wxWrapSizer;
namespace sigflow::virtual_lab {
class VirtualBoardPanel:public wxPanel {
public:
    struct ExampleEntry {
        std::string id;
        std::string label;
        std::string relative_project_path;
    };
    static const std::vector<ExampleEntry>& Examples();
    VirtualBoardPanel(wxWindow*,std::shared_ptr<VirtualSessionController>);
    ~VirtualBoardPanel();
    void SetProjectProvider(std::function<eda::Json()> f){project_provider_=std::move(f);}
    void SetBuildDirOverride(const std::filesystem::path& buildDir){build_dir_override_=buildDir;}
    void LoadExample(const std::string& id);
    std::string SelectedExampleId()const;
    // Host-installed callback: pops this live panel out into the independent
    // floating window without touching the shared session controller.
    void SetFloatOutHandler(std::function<void()> f);
    void InvokeControl(const std::string&);
    void SetContact(int pad,bool pressed){Contact(pad,pressed);}
    std::string DetailText()const;
    void SetStepMode(const std::string&,const std::string& ticks="1000000");
    void SetIntegratedView(bool);
    bool IntegratedView()const;
    void SetDip(const std::string&,unsigned value);
    void ToggleKeypadKey(const std::string&,unsigned row,unsigned col);
    void SetKeys(const std::string&,const eda::Json& keys);
    eda::Json KeypadKeys(const std::string&)const;
    eda::Json InspectNode(const std::string&,bool showPopup=true);
private:
    void Update();
    void Load();
    void Contact(int pad,bool pressed);
    void ReleaseContact();
    void ApplyTheme();
    std::shared_ptr<VirtualSessionController> controller_;
    wxPanel* drawing_=nullptr;wxStaticText* status_=nullptr;
    wxTextCtrl* detail_=nullptr;wxListCtrl* diagnostics_=nullptr;wxSpinCtrl* replay_count_=nullptr;
    std::function<eda::Json()> project_provider_;
    std::filesystem::path build_dir_override_;
    std::function<void()> float_out_handler_;wxButton* float_out_=nullptr;wxSizer* toolbar_=nullptr;
    wxChoice* example_picker_=nullptr;
    std::string diagnostic_signature_;
    wxPanel* phase1_controls_=nullptr;wxChoice *step_mode_=nullptr,*view_mode_=nullptr;wxTextCtrl* step_ticks_=nullptr;
    void Step();
    std::map<std::string,wxButton*> controls_;
    size_t subscription_=0;
    int held_pad_=-1;
    UiTheme::Subscription theme_;
    std::vector<wxWeakRef<wxDialog>> inspections_;std::string inspection_run_;
};
}
