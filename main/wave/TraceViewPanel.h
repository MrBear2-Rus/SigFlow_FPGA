#pragma once

#include "../trace/TraceSource.h"
#include "../debug/WaveformComparator.h"
#include "../debug/DebugBehaviorSummary.h"
#include "WavePatternSearch.h"
#include "WaveformView.h"
#include "WaveUartLane.h"
#include "WaveProtocolDecoder.h"
#include "WaveProtocolEventReport.h"
#include "../trace/TraceQueryService.h"

#include "ui/UiTheme.h"   // T-140：主题订阅句柄（析构自动解绑，THE-028）

#include <functional>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <string>
#include <utility>
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
    // 将 TB-14 协议事务追加到当前工作台事件时间线；事务仍保留原始起止时间，
    // WaveEvent 以起点作为可跳转锚点，并在标签中显示结束时间/有效性。
    void InjectProtocolEvents(const std::vector<ProtocolTransaction>& transactions);
    bool InjectProtocolEventReport(const ProtocolEventReport& report, std::string& error);

    // Compare Hub 联动。
    void RegisterCompareHub();
    void UnregisterCompareHub();

    // T-140 / THE-030：把本面板内"写一次的 Set*Colour"抽成可重施入口。
    // 只重写 m_measureLabel 前景 + UART 无效帧行色 + 转发给内嵌 WaveformView；
    // 不重建 UI、不动波形/事件/UART 数据、不做重布局。
    void ReapplyTheme();

private:
    struct SignalTreeData : public wxClientData {
        explicit SignalTreeData(int id) : signalId(id) {}
        int signalId = -1;
    };

    struct ModuleTreeData : public wxClientData {
        explicit ModuleTreeData(std::string value) : scope(std::move(value)) {}
        std::string scope;
        bool populated = false;
    };

    struct TreePlaceholderData : public wxClientData {};

    void BuildUi();
    void InstallTrace(std::shared_ptr<sigflow::trace::TraceSource> source,
                      const WaveSourceIdentity& identity);
    void PopulateTree();
    void PopulateModule(const wxTreeListItem& item);
    void AddTreePlaceholder(const wxTreeListItem& item);
    void ScheduleVisiblePrefetch();
    static std::string ModuleLabel(const std::string& scope);
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
    void OnTreeItemExpanding(wxTreeListEvent& event);
    void OnTreeItemActivated(wxTreeListEvent& event);
    void OnTreeContextMenu(wxTreeListEvent& event);
    void OnSearchChanged(wxCommandEvent& event);
    void OnEventFilterChanged(wxCommandEvent& event);
    void OnEventActivated(wxListEvent& event);
    void OnFindPrev(wxCommandEvent& event);
    void OnFindNext(wxCommandEvent& event);
    void OnLoadUart(wxCommandEvent& event);
    void OnLoadProtocolReport(wxCommandEvent& event);
    void OnSaveProtocolReport(wxCommandEvent& event);
    void OnUartActivated(wxListEvent& event);
    void RefreshUartLane();

    std::shared_ptr<sigflow::trace::TraceSource> m_source;
    sigflow::trace::TraceQueryService m_queryService;
    std::uint64_t m_prefetchJobId = 0;
    WaveformView* m_view = nullptr;
    wxTreeListCtrl* m_tree = nullptr;
    wxTextCtrl* m_searchBox = nullptr;
    wxListCtrl* m_eventList = nullptr;
    wxTextCtrl* m_eventFilterBox = nullptr;
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
    ProtocolEventReport m_lastProtocolReport;
    bool m_hasProtocolReport = false;
    std::vector<std::size_t> m_eventRows;
    wxButton* m_themeButton = nullptr;
    wxButton* m_unresolvedButton = nullptr;
    WaveSourceIdentity m_sourceIdentity;
    std::vector<WaveSignalBinding> m_sessionBindings;
    std::vector<WaveSignalBinding> m_unresolvedBindings;
    WaveNavigationCallback m_navigationCallback;
    UiTheme::Subscription m_themeSubscription;
};

} // namespace wave
} // namespace sigflow
