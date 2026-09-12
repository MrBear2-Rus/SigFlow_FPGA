#pragma once

#include "DebugContract.h"

#include <functional>

#include <wx/frame.h>

class wxButton;
class wxCheckBox;
class wxChoice;
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

    wxChoice* protocolChoice_ = nullptr;
    wxChoice* baudChoice_ = nullptr;
    wxTextCtrl* txPortCtrl_ = nullptr;
    wxTextCtrl* rxPortCtrl_ = nullptr;
    wxSpinCtrl* txPinCtrl_ = nullptr;
    wxSpinCtrl* rxPinCtrl_ = nullptr;
    wxSpinCtrl* rstPinCtrl_ = nullptr;
    wxCheckBox* syncEnabledCtrl_ = nullptr;
};
