#pragma once

#include "DebugContract.h"

#include <functional>
#include <vector>

#include <wx/frame.h>

#include "ui/UiTheme.h"   // T-140 / THE-030：子窗主题刷新

class wxButton;
class wxCheckBox;
class wxChoice;
class wxNotebook;
class wxPanel;
class wxSpinCtrl;
class wxStaticText;
class wxTextCtrl;

class DebugContractConfigWindow : public wxFrame {
public:
    explicit DebugContractConfigWindow(wxWindow* parent);

    void SetProjectContext(const wxString& projectPath);
    void Reload();
    void SetSavedHandler(std::function<void(const wxString&)> handler);

private:
    void ApplyTheme();

    void BuildUi();
    void LoadDefaultContract();
    void LoadFromPath(const wxString& path);
    bool BuildContract(sigflow::debug::DebugContract& contract, wxString& error) const;
    bool SaveToPath(const wxString& path, wxString& error);
    void ApplyContractToUi();
    void SetStatus(const wxString& message, bool error = false);
    wxString DefaultContractPath() const;

    void OnOpen(wxCommandEvent& event);
    void OnReload(wxCommandEvent& event);
    void OnSave(wxCommandEvent& event);
    void OnSaveAs(wxCommandEvent& event);
    void OnValidate(wxCommandEvent& event);

    // T-140 / THE-030：主题刷新（THE-028：订阅句柄析构即解绑）
    UiTheme::Subscription themeSubscription_;
    wxNotebook* notebook_ = nullptr;
    std::vector<wxPanel*> pages_;
    bool statusIsError_ = false;

    wxString projectPath_;
    wxString contractFilePath_;
    sigflow::debug::DebugContract contract_;
    std::function<void(const wxString&)> savedHandler_;

    wxStaticText* pathLabel_ = nullptr;
    wxStaticText* statusLabel_ = nullptr;
    wxTextCtrl* targetProfileCtrl_ = nullptr;
    wxTextCtrl* topModuleCtrl_ = nullptr;
    wxTextCtrl* sampleClockCtrl_ = nullptr;
    wxTextCtrl* sampleFrequencyCtrl_ = nullptr;
    wxTextCtrl* probeEditor_ = nullptr;

    wxChoice* triggerChoice_ = nullptr;
    wxTextCtrl* triggerMaskCtrl_ = nullptr;
    wxTextCtrl* triggerValueCtrl_ = nullptr;
    wxSpinCtrl* triggerCyclesCtrl_ = nullptr;
    wxTextCtrl* hsValidCtrl_ = nullptr;
    wxTextCtrl* hsReadyCtrl_ = nullptr;

    wxSpinCtrl* depthCtrl_ = nullptr;
    wxSpinCtrl* pretriggerCtrl_ = nullptr;
    wxSpinCtrl* decimationCtrl_ = nullptr;

    wxChoice* transportKindChoice_ = nullptr;
    wxChoice* protocolChoice_ = nullptr;
    wxTextCtrl* protocolVersionCtrl_ = nullptr;
    wxChoice* baudChoice_ = nullptr;
    wxTextCtrl* txPortCtrl_ = nullptr;
    wxTextCtrl* rxPortCtrl_ = nullptr;
    wxSpinCtrl* txPinCtrl_ = nullptr;
    wxSpinCtrl* rxPinCtrl_ = nullptr;
    wxSpinCtrl* rstPinCtrl_ = nullptr;
    wxSpinCtrl* tckHzCtrl_ = nullptr;
    wxTextCtrl* probeIdCtrl_ = nullptr;
    wxTextCtrl* targetIdCtrl_ = nullptr;
    wxSpinCtrl* interfaceIndexCtrl_ = nullptr;
    wxCheckBox* syncEnabledCtrl_ = nullptr;
};
