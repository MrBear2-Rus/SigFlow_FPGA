#pragma once

// Model/API dashboard dialog.  The dialog is a pure view over host-provided
// callbacks: it never reads the credential store, restarts the sidecar or
// talks to the network itself.

#include "agent/AgentModelSettings.h"
#include "ui/UiTheme.h"

#include <json/json.h>
#include <wx/dialog.h>
#include <wx/string.h>
#include <wx/timer.h>

#include <functional>
#include <string>

class wxScrolledWindow;
class wxStaticText;
class wxTextCtrl;

namespace agent::gui {

namespace controls {
class AgentButton;
}

class AgentDashboardDialog final : public wxDialog
{
public:
    struct Host
    {
        std::function<ModelSettings()> settings;
        std::function<bool()> ready;
        std::function<bool()> modelAvailable;
        std::function<bool()> restartPossible;
        std::function<wxString()> serviceText;
        /// Sidecar state id: "ready"/"disabled"/"starting"/"backing-off"/
        /// "failed"/"stopped".  Used to end restart polling on a terminal state.
        std::function<wxString()> serviceState;
        std::function<bool(const ModelSettings&, std::string*)> save;
        std::function<bool(std::string*)> clear;
        /// Start one usage+balance query; false when the service is not ready
        /// or a previous query is still running.
        std::function<bool()> refresh;
    };

    AgentDashboardDialog(wxWindow* parent, Host host);
    ~AgentDashboardDialog() override;

    // Query result entry points.  The host calls ShowUsage/ShowBalance (or the
    // matching failure) for both halves, then EndQuery() exactly once.
    void ShowUsage(const Json::Value& usage);
    void ShowUsageFailure(const std::string& code, const std::string& message);
    void ShowBalance(const Json::Value& balance);
    void ShowBalanceFailure(const std::string& code, const std::string& message);
    void EndQuery();
    /// The query result belongs to a previous sidecar generation.
    void CancelQuery(const wxString& message);

    void ShowSaveResult(bool ok, const wxString& message);

private:
    void BuildUi();
    void ApplyTheme();
    void RefreshStatus();
    void RequestQuery();
    void BeginRestartPolling();
    void SetQueryStatus(const wxString& text, bool force = false);
    void OnSave(wxCommandEvent& event);
    void OnClear(wxCommandEvent& event);
    void OnRefresh(wxCommandEvent& event);
    void OnTimer(wxTimerEvent& event);
    void OnSize(wxSizeEvent& event);

    Host host_;
    wxScrolledWindow* m_scroll = nullptr;
    wxStaticText* m_status = nullptr;
    wxTextCtrl* m_key = nullptr;
    wxTextCtrl* m_model = nullptr;
    wxTextCtrl* m_baseUrl = nullptr;
    wxStaticText* m_keyNote = nullptr;
    wxTextCtrl* m_balance = nullptr;
    wxTextCtrl* m_usage = nullptr;
    wxStaticText* m_queryStatus = nullptr;
    controls::AgentButton* m_save = nullptr;
    controls::AgentButton* m_clear = nullptr;
    controls::AgentButton* m_refresh = nullptr;
    controls::AgentButton* m_close = nullptr;
    wxTimer m_pollTimer;
    UiTheme::Subscription m_theme;
    wxString m_lastStatusText;
    wxString m_lastQueryStatus;
    wxString m_queryUsageError;
    wxString m_queryBalanceError;
    int m_lastWrapWidth = -1;
    int m_pollTicks = 0;
    bool m_queryPending = false;
    bool m_queryUsageOk = false;
    bool m_queryBalanceOk = false;
    bool m_restarting = false;
};

}  // namespace agent::gui
