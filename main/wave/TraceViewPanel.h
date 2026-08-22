#pragma once

#include "../trace/TraceSource.h"
#include "WavePatternSearch.h"
#include "WaveformView.h"

#include <memory>
#include <string>
#include <vector>

#include <wx/listctrl.h>
#include <wx/panel.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/treectrl.h>

namespace sigflow {
namespace wave {

class WaveformView;

// W3 宿主面板：信号树浏览 + 波形视图 + 事件列表 + 模式搜索 + 会话。
class TraceViewPanel : public wxPanel {
public:
    explicit TraceViewPanel(wxWindow* parent);

    bool OpenTrace(const std::string& path);
    void SaveSession(const std::string& path);
    bool LoadSession(const std::string& path);
    void SetSessionDir(const std::string& dir) { m_sessionDir = dir; }

    WaveformView* View() { return m_view; }

private:
    struct SignalTreeData : public wxTreeItemData {
        explicit SignalTreeData(int id) : signalId(id) {}
        int signalId = -1;
    };

    void BuildUi();
    void PopulateTree();
    void ToggleSignal(int signalId);
    void RefreshEvents();
    void RefreshMeasurement();
    void HighlightNearestEvent();
    void RunPatternSearch(bool forward);
    bool ParsePattern(const std::string& text, WavePatternSpec& spec) const;

    void OnOpenTrace(wxCommandEvent& event);
    void OnSaveSession(wxCommandEvent& event);
    void OnLoadSession(wxCommandEvent& event);
    void OnZoomIn(wxCommandEvent& event);
    void OnZoomOut(wxCommandEvent& event);
    void OnZoomReset(wxCommandEvent& event);
    void OnClearMarkers(wxCommandEvent& event);
    void OnClearAB(wxCommandEvent& event);
    void OnTreeActivated(wxTreeEvent& event);
    void OnSearchChanged(wxCommandEvent& event);
    void OnEventActivated(wxListEvent& event);
    void OnFindPrev(wxCommandEvent& event);
    void OnFindNext(wxCommandEvent& event);

    std::shared_ptr<sigflow::trace::TraceSource> m_source;
    WaveformView* m_view = nullptr;
    wxTreeCtrl* m_tree = nullptr;
    wxTextCtrl* m_searchBox = nullptr;
    wxListCtrl* m_eventList = nullptr;
    wxStaticText* m_measureLabel = nullptr;
    wxTextCtrl* m_patternBox = nullptr;
    std::string m_sessionDir;
};

} // namespace wave
} // namespace sigflow
