#pragma once

#include "../trace/TraceSource.h"
#include "../debug/WaveformComparator.h"
#include "../debug/DebugBehaviorSummary.h"
#include "WavePatternSearch.h"
#include "WaveformView.h"
#include "WaveUartLane.h"

#include <functional>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <string>
#include <vector>

#include <wx/listctrl.h>
#include <wx/button.h>
#include <wx/panel.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/treelist.h>

namespace sigflow {
namespace wave {

class WaveformView;

using WaveNavigationCallback =
    std::function<void(const std::string& signalName,
                       const std::string& sourcePath,
                       int sourceLine,
                       sigflow::trace::TimeValue time)>;

// W3 宿主面板：信号树浏览 + 波形视图 + 事件列表 + 模式搜索 + 会话。
class TraceViewPanel : public wxPanel {
public:
    explicit TraceViewPanel(wxWindow* parent);

    bool OpenTrace(const std::string& path);
    void AutoLoadTrace();
    void SaveSession(const std::string& path);
    bool LoadSession(const std::string& path);
    bool LoadUartCapture(const std::string& path);
    bool AddSignalByName(const std::string& name);
    void SetSessionDir(const std::string& dir) { m_sessionDir = dir; }
    void SetProjectPath(const std::string& path) { m_projectPath = path; }
    void SetNavigationCallback(WaveNavigationCallback callback)
    {
        m_navigationCallback = std::move(callback);
    }

    WaveformView* View() { return m_view; }

    // ── W4：Compare / 比对事件注入 ──
    // 将 compare 结果注入为事件（锚点 + 首差异）。isHwPanel=true 用 hw 时间戳，false 用 sim。
    void InjectCompareEvents(const sigflow::debug::ComparisonResult& r, bool isHwPanel);
    void InjectBehaviorEvents(const std::vector<sigflow::debug::DebugBehaviorEvent>& events,
                              bool isHwPanel, std::int64_t timeOffset = 0);

    // Compare Hub 联动。
    void RegisterCompareHub();
    void UnregisterCompareHub();

private:
    struct SignalTreeData : public wxClientData {
        explicit SignalTreeData(int id) : signalId(id) {}
        int signalId = -1;
    };

    void BuildUi();
    void PopulateTree();
    void SetSignalChecked(int signalId, bool checked);
    void RefreshEvents();
    void RefreshMeasurement();
    void HighlightNearestEvent();
    void RunPatternSearch(bool forward);
    bool ParsePattern(const std::string& text, WavePatternSpec& spec) const;

    void OnOpenTrace(wxCommandEvent& event);
    void OnAutoLoadTrace(wxCommandEvent& event);
    void OnSaveSession(wxCommandEvent& event);
    void OnLoadSession(wxCommandEvent& event);
    void OnZoomIn(wxCommandEvent& event);
    void OnZoomOut(wxCommandEvent& event);
    void OnZoomReset(wxCommandEvent& event);
    void OnClearMarkers(wxCommandEvent& event);
    void OnClearAB(wxCommandEvent& event);
    void OnTreeItemChecked(wxTreeListEvent& event);
    void OnTreeItemActivated(wxTreeListEvent& event);
    void OnTreeContextMenu(wxTreeListEvent& event);
    void OnSearchChanged(wxCommandEvent& event);
    void OnEventActivated(wxListEvent& event);
    void OnFindPrev(wxCommandEvent& event);
    void OnFindNext(wxCommandEvent& event);
    void OnLoadUart(wxCommandEvent& event);
    void OnUartActivated(wxListEvent& event);
    void RefreshUartLane();

    std::shared_ptr<sigflow::trace::TraceSource> m_source;
    WaveformView* m_view = nullptr;
    wxTreeListCtrl* m_tree = nullptr;
    wxTextCtrl* m_searchBox = nullptr;
    wxListCtrl* m_eventList = nullptr;
    wxListCtrl* m_uartList = nullptr;
    wxStaticText* m_measureLabel = nullptr;
    wxTextCtrl* m_patternBox = nullptr;
    std::string m_sessionDir;
    std::string m_projectPath;
    bool m_compareRegistered = false;
    std::unordered_map<int, std::string> m_signalAliases;
    std::unordered_map<int, std::string> m_signalComments;
    std::vector<WaveUartFrame> m_uartFrames;
    std::string m_uartCapturePath;
    wxButton* m_themeButton = nullptr;
    WaveNavigationCallback m_navigationCallback;
};

} // namespace wave
} // namespace sigflow
