#include "TraceViewPanel.h"

#include "WaveAnalysis.h"
#include "WaveCompareHub.h"
#include "WavePatternSearch.h"
#include "WaveSession.h"
#include "../trace/TraceCache.h"
#include "../trace/VcdLazyTraceSource.h"

#include <wx/button.h>
#include <wx/choicdlg.h>
#include <wx/dir.h>
#include <wx/filedlg.h>
#include <wx/filename.h>
#include <wx/menu.h>
#include <wx/msgdlg.h>
#include <wx/sizer.h>
#include <wx/splitter.h>
#include <wx/textdlg.h>

#include <algorithm>
#include <sstream>

namespace sigflow {
namespace wave {

TraceViewPanel::TraceViewPanel(wxWindow* parent)
    : wxPanel(parent, wxID_ANY)
{
    BuildUi();
}

void TraceViewPanel::InjectCompareEvents(const sigflow::debug::ComparisonResult& r,
                                          bool isHwPanel)
{
    if (!m_view) return;
    std::vector<WaveEvent> events;

    // 对齐锚点事件（绿色）
    if (r.aligned) {
        const sigflow::trace::TimeValue anchorTime =
            static_cast<sigflow::trace::TimeValue>(isHwPanel ? r.alignment.hwAnchorTime
                                                              : r.alignment.simAnchorTime);
        std::string anchorLabel = "Anchor: " + r.alignment.anchorSignal;
        switch (r.alignment.anchorKind) {
            case sigflow::debug::AnchorKind::ResetRelease:   anchorLabel += " (ResetRelease)"; break;
            case sigflow::debug::AnchorKind::TriggerHit:     anchorLabel += " (Trigger)"; break;
            case sigflow::debug::AnchorKind::InputTxn:       anchorLabel += " (InputTxn)"; break;
        }
        events.push_back(WaveEvent{anchorTime, anchorLabel, 0xFF10B981}); // 绿色
    }

    // 每个信号的首个差异（红色）
    for (const sigflow::debug::WaveformDiff& diff : r.firstDiffs) {
        const sigflow::trace::TimeValue t =
            static_cast<sigflow::trace::TimeValue>(isHwPanel ? diff.hwTime : diff.simTime);
        std::ostringstream oss;
        oss << "DIFF " << diff.signalName << ": exp=" << diff.expectedValue
            << " got=" << diff.actualValue
            << " (" << static_cast<int>(diff.confidence * 100) << "%)";
        WaveEvent event{t, oss.str(), 0xFFEF4444};
        event.signalName = diff.signalName;
        event.sourcePath = diff.location.sourcePath;
        event.sourceLine = diff.location.sourceLine;
        events.push_back(std::move(event)); // 红色
    }

    // 汇总差异（橙色，放在首差异时间）
    if (!r.firstDiffs.empty()) {
        const sigflow::debug::WaveformDiff& first = r.firstDiffs.front();
        const sigflow::trace::TimeValue t =
            static_cast<sigflow::trace::TimeValue>(isHwPanel ? first.hwTime : first.simTime);
        std::ostringstream oss;
        oss << "SUMMARY: " << r.totalDiffs << " diff(s) in " << r.totalSignalsCompared
            << " signal(s) — first: " << first.signalName;
        WaveEvent event{t, oss.str(), 0xFFF97316};
        event.signalName = first.signalName;
        events.push_back(std::move(event)); // 橙色
    }

    // 按时间排序
    std::sort(events.begin(), events.end(),
              [](const WaveEvent& a, const WaveEvent& b) { return a.time < b.time; });

    m_view->SetEvents(events);
    RefreshEvents();
}

void TraceViewPanel::InjectBehaviorEvents(
    const std::vector<sigflow::debug::DebugBehaviorEvent>& behaviorEvents,
    bool isHwPanel, std::int64_t timeOffset)
{
    if (!m_view) return;
    std::vector<WaveEvent> events = m_view->Events();
    for (const auto& behavior : behaviorEvents) {
        WaveEvent event;
        const std::int64_t translated = static_cast<std::int64_t>(behavior.time) +
                                        (isHwPanel ? 0 : timeOffset);
        event.time = static_cast<sigflow::trace::TimeValue>(translated < 0 ? 0 : translated);
        event.signalName = behavior.signalName;
        event.label = behavior.kind + (behavior.signalName.empty() ? "" : " " + behavior.signalName);
        if (!behavior.message.empty()) event.label += ": " + behavior.message;
        event.label += " [" + (isHwPanel ? std::string("HW") : std::string("Sim")) + "]";
        if (behavior.kind == "first_difference") event.color = 0xFFEF4444;
        else if (behavior.kind == "trigger_hit") event.color = 0xFFF97316;
        else if (behavior.kind == "reset_release") event.color = 0xFF10B981;
        else event.color = 0xFF60A5FA;
        events.push_back(std::move(event));
    }
    std::sort(events.begin(), events.end(),
              [](const WaveEvent& left, const WaveEvent& right) {
                  if (left.time != right.time) return left.time < right.time;
                  return left.label < right.label;
              });
    m_view->SetEvents(events);
    RefreshEvents();
}

void TraceViewPanel::RegisterCompareHub()
{
    if (!m_view || m_compareRegistered) return;
    WaveCompareHub::Register(m_view);
    WaveCompareHub::SetLinkTimeView(true);
    WaveCompareHub::SetLinkPlayheads(true);
    m_compareRegistered = true;
}

void TraceViewPanel::UnregisterCompareHub()
{
    if (!m_view || !m_compareRegistered) return;
    WaveCompareHub::Unregister(m_view);
    m_compareRegistered = false;
}

void TraceViewPanel::BuildUi()
{
    wxBoxSizer* root = new wxBoxSizer(wxVERTICAL);

    // 工具栏
    wxBoxSizer* toolbar = new wxBoxSizer(wxHORIZONTAL);
    wxButton* open = new wxButton(this, wxID_ANY, "Open VCD");
    wxButton* autoLoad = new wxButton(this, wxID_ANY, "Auto Load");
    wxButton* save = new wxButton(this, wxID_ANY, "Save Session");
    wxButton* load = new wxButton(this, wxID_ANY, "Load Session");
    wxButton* zoomIn = new wxButton(this, wxID_ANY, "Zoom+");
    wxButton* zoomOut = new wxButton(this, wxID_ANY, "Zoom-");
    wxButton* zoomReset = new wxButton(this, wxID_ANY, "Fit");
    wxButton* clearMarkers = new wxButton(this, wxID_ANY, "Clear Markers");
    wxButton* clearAB = new wxButton(this, wxID_ANY, "Clear A-B");
    wxButton* uart = new wxButton(this, wxID_ANY, "Load UART lane");
    m_themeButton = new wxButton(this, wxID_ANY, "Theme: Dark");
    for (wxButton* button : { open, autoLoad, save, load, zoomIn, zoomOut, zoomReset,
                              clearMarkers, clearAB, uart, m_themeButton }) {
        toolbar->Add(button, 0, wxALL, 2);
    }
    toolbar->AddStretchSpacer();
    m_measureLabel = new wxStaticText(this, wxID_ANY, wxEmptyString);
    m_measureLabel->SetForegroundColour(wxColour(148, 163, 184));
    toolbar->Add(m_measureLabel, 0, wxALIGN_CENTER_VERTICAL | wxALL, 4);
    root->Add(toolbar, 0, wxEXPAND);

    // 主区：左侧信号树 / 右侧波形
    wxSplitterWindow* splitter = new wxSplitterWindow(this, wxID_ANY,
        wxDefaultPosition, wxDefaultSize, wxSP_LIVE_UPDATE | wxBORDER_NONE);
    wxPanel* left = new wxPanel(splitter);
    wxBoxSizer* leftLayout = new wxBoxSizer(wxVERTICAL);
    m_searchBox = new wxTextCtrl(left, wxID_ANY, wxEmptyString, wxDefaultPosition,
                                 wxDefaultSize, wxTE_PROCESS_ENTER);
    leftLayout->Add(m_searchBox, 0, wxEXPAND | wxALL, 2);
    m_tree = new wxTreeListCtrl(left, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                            wxTL_CHECKBOX | wxTL_NO_HEADER);
    m_tree->AppendColumn("Signal");
    leftLayout->Add(m_tree, 1, wxEXPAND | wxALL, 2);
    left->SetSizer(leftLayout);
    left->SetMinSize(wxSize(220, -1));

    m_view = new WaveformView(splitter, true);
    splitter->SplitVertically(left, m_view, 220);
    splitter->SetMinimumPaneSize(160);
    root->Add(splitter, 1, wxEXPAND);

    // 底部：事件列表 + 模式搜索
    wxBoxSizer* bottom = new wxBoxSizer(wxHORIZONTAL);
    m_eventList = new wxListCtrl(this, wxID_ANY, wxDefaultPosition, wxSize(360, 110),
                                 wxLC_REPORT | wxLC_SINGLE_SEL | wxBORDER_SIMPLE);
    m_eventList->AppendColumn("Time", wxLIST_FORMAT_LEFT, 90);
    m_eventList->AppendColumn("Event", wxLIST_FORMAT_LEFT, 240);
    bottom->Add(m_eventList, 1, wxEXPAND | wxALL, 2);

    m_uartList = new wxListCtrl(this, wxID_ANY, wxDefaultPosition, wxSize(360, 110),
                                wxLC_REPORT | wxLC_SINGLE_SEL | wxBORDER_SIMPLE);
    m_uartList->AppendColumn("Offset", wxLIST_FORMAT_LEFT, 70);
    m_uartList->AppendColumn("Frame", wxLIST_FORMAT_LEFT, 90);
    m_uartList->AppendColumn("Details", wxLIST_FORMAT_LEFT, 220);
    bottom->Add(m_uartList, 1, wxEXPAND | wxALL, 2);

    wxBoxSizer* searchCol = new wxBoxSizer(wxVERTICAL);
    searchCol->Add(new wxStaticText(this, wxID_ANY, "Pattern (name:kind[:value], e.g. clk:rise,cnt:0010)"),
                   0, wxLEFT | wxTOP, 2);
    wxBoxSizer* searchRow = new wxBoxSizer(wxHORIZONTAL);
    m_patternBox = new wxTextCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition,
                                  wxDefaultSize, wxTE_PROCESS_ENTER);
    wxButton* prev = new wxButton(this, wxID_ANY, "Prev");
    wxButton* next = new wxButton(this, wxID_ANY, "Next");
    searchRow->Add(m_patternBox, 1, wxEXPAND);
    searchRow->Add(prev, 0, wxLEFT, 2);
    searchRow->Add(next, 0, wxLEFT, 2);
    searchCol->Add(searchRow, 1, wxEXPAND | wxALL, 2);
    bottom->Add(searchCol, 1, wxEXPAND);
    root->Add(bottom, 0, wxEXPAND);

    SetSizer(root);

    open->Bind(wxEVT_BUTTON, &TraceViewPanel::OnOpenTrace, this);
    autoLoad->Bind(wxEVT_BUTTON, &TraceViewPanel::OnAutoLoadTrace, this);
    save->Bind(wxEVT_BUTTON, &TraceViewPanel::OnSaveSession, this);
    load->Bind(wxEVT_BUTTON, &TraceViewPanel::OnLoadSession, this);
    zoomIn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { m_view->ZoomIn(); });
    zoomOut->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { m_view->ZoomOut(); });
    zoomReset->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { m_view->ZoomReset(); });
    clearMarkers->Bind(wxEVT_BUTTON,
                       [this](wxCommandEvent&) { m_view->ClearMarkers(); });
    clearAB->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { m_view->ClearAB(); });
    uart->Bind(wxEVT_BUTTON, &TraceViewPanel::OnLoadUart, this);
    m_themeButton->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        m_view->SetTheme(m_view->Theme() == WaveTheme::Dark ? WaveTheme::Light : WaveTheme::Dark);
        m_themeButton->SetLabel(m_view->Theme() == WaveTheme::Dark ? "Theme: Dark" : "Theme: Light");
    });
    m_tree->Bind(wxEVT_TREELIST_ITEM_CHECKED, &TraceViewPanel::OnTreeItemChecked, this);
    m_tree->Bind(wxEVT_TREELIST_ITEM_ACTIVATED, &TraceViewPanel::OnTreeItemActivated, this);
    m_tree->Bind(wxEVT_TREELIST_ITEM_CONTEXT_MENU,
                 &TraceViewPanel::OnTreeContextMenu, this);
    m_searchBox->Bind(wxEVT_TEXT, &TraceViewPanel::OnSearchChanged, this);
    m_eventList->Bind(wxEVT_LIST_ITEM_ACTIVATED, &TraceViewPanel::OnEventActivated, this);
    m_uartList->Bind(wxEVT_LIST_ITEM_ACTIVATED, &TraceViewPanel::OnUartActivated, this);
    prev->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { RunPatternSearch(false); });
    next->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { RunPatternSearch(true); });
    m_patternBox->Bind(wxEVT_TEXT_ENTER, [this](wxCommandEvent&) { RunPatternSearch(true); });

    m_view->SetOnViewChanged([this]() {
        RefreshMeasurement();
        HighlightNearestEvent();
    });
}

bool TraceViewPanel::OpenTrace(const std::string& path)
{
    auto inner = std::make_unique<sigflow::trace::VcdLazyTraceSource>();
    auto source = std::make_shared<sigflow::trace::CachingTraceSource>(
        std::move(inner), 64 * 1024 * 1024);
    std::string error;
    if (!source->Open(path, error)) {
        wxMessageBox(error, "Open Trace", wxOK | wxICON_ERROR, this);
        return false;
    }
    m_source = source;
    m_view->SetTraceSource(m_source);
    m_view->SetVisibleSignals({});
    PopulateTree();
    RefreshEvents();
    RefreshUartLane();
    m_measureLabel->SetLabel(
        "Loaded " + std::to_string(m_source->Signals().size()) +
        " signal(s), range 0.." + std::to_string(m_source->TimeRange().end));
    return true;
}

bool TraceViewPanel::LoadUartCapture(const std::string& path)
{
    std::string error;
    m_uartFrames = WaveUartLane::DecodeFile(path, error);
    if (!error.empty()) {
        wxMessageBox(error, "UART Lane", wxOK | wxICON_ERROR, this);
        return false;
    }
    m_uartCapturePath = path;
    RefreshUartLane();
    return true;
}

void TraceViewPanel::RefreshUartLane()
{
    if (!m_uartList) return;
    m_uartList->DeleteAllItems();
    for (const WaveUartFrame& frame : m_uartFrames) {
        const long row = m_uartList->InsertItem(m_uartList->GetItemCount(),
                                                std::to_string(frame.offset));
        m_uartList->SetItem(row, 1, frame.label);
        m_uartList->SetItem(row, 2, frame.detail);
        if (!frame.valid) m_uartList->SetItemTextColour(row, wxColour(220, 38, 38));
    }
}

void TraceViewPanel::OnLoadUart(wxCommandEvent&)
{
    wxFileDialog dialog(this, "Load UART Capture", m_sessionDir, wxEmptyString,
                        "Binary captures (*.raw;*.bin)|*.raw;*.bin|All files (*.*)|*.*",
                        wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    if (dialog.ShowModal() == wxID_OK) LoadUartCapture(dialog.GetPath().ToStdString());
}

void TraceViewPanel::OnUartActivated(wxListEvent& event)
{
    const long index = event.GetIndex();
    if (index < 0 || static_cast<std::size_t>(index) >= m_uartFrames.size()) return;
    const auto& frame = m_uartFrames[static_cast<std::size_t>(index)];
    if (!m_source || !frame.valid) return;
    const auto time = static_cast<sigflow::trace::TimeValue>(frame.offset);
    m_view->JumpToTime(time);
    m_view->SetPlayhead(time);
}

void TraceViewPanel::PopulateTree()
{
    m_tree->DeleteAllItems();
    const wxTreeListItem root = m_tree->GetRootItem();
    if (!m_source) return;

    const std::string filter = m_searchBox->GetValue().Lower().ToStdString();
    for (const sigflow::trace::SignalInfo& signal : m_source->Signals()) {
        if (!filter.empty() &&
            signal.name.find(filter) == std::string::npos &&
            signal.fullName.find(filter) == std::string::npos) {
            continue;
        }
        // 按 scope 分段建模块节点
        wxTreeListItem parent = root;
        std::string scope = signal.scope;
        while (!scope.empty()) {
            const std::size_t slash = scope.find('/');
            const std::string part = scope.substr(0, slash);
            wxTreeListItem child = m_tree->GetFirstChild(parent);
            wxTreeListItem match;
            while (child.IsOk()) {
                if (m_tree->GetItemText(child) == wxString::FromUTF8(part)) {
                    match = child;
                    break;
                }
                child = m_tree->GetNextSibling(child);
            }
            if (!match.IsOk()) {
                match = m_tree->AppendItem(parent, wxString::FromUTF8(part));
            }
            parent = match;
            scope = (slash == std::string::npos) ? std::string() : scope.substr(slash + 1);
        }
        std::string displayName = signal.name;
        const auto alias = m_signalAliases.find(signal.id);
        if (alias != m_signalAliases.end() && !alias->second.empty()) {
            displayName += " [" + alias->second + "]";
        }
        wxTreeListItem leaf = m_tree->AppendItem(parent, wxString::FromUTF8(displayName));
        m_tree->SetItemData(leaf, new SignalTreeData(signal.id));
        const WaveViewState& state = m_view->State();
        if (std::find(state.visibleSignalIds.begin(), state.visibleSignalIds.end(), signal.id) !=
            state.visibleSignalIds.end()) {
            m_tree->CheckItem(leaf);
        }
    }
    for (wxTreeListItem item = m_tree->GetFirstItem(); item.IsOk();
         item = m_tree->GetNextItem(item)) {
        if (m_tree->GetFirstChild(item).IsOk()) m_tree->Expand(item);
    }
}

void TraceViewPanel::SetSignalChecked(int signalId, bool checked)
{
    if (!m_view) return;
    WaveViewState state = m_view->State();
    const auto it = std::find(state.visibleSignalIds.begin(),
                              state.visibleSignalIds.end(), signalId);
    if (checked && it == state.visibleSignalIds.end()) {
        state.visibleSignalIds.push_back(signalId);
    } else if (!checked && it != state.visibleSignalIds.end()) {
        state.visibleSignalIds.erase(it);
    }
    m_view->SetVisibleSignals(state.visibleSignalIds);
}

void TraceViewPanel::RefreshEvents()
{
    m_eventList->DeleteAllItems();
    for (const WaveEvent& event : m_view->Events()) {
        const long item = m_eventList->InsertItem(m_eventList->GetItemCount(),
                                                  std::to_string(event.time));
        m_eventList->SetItem(item, 1, event.label);
    }
}

void TraceViewPanel::RefreshMeasurement()
{
    const std::string text = m_view->MeasurementText();
    if (!text.empty()) {
        m_measureLabel->SetLabel(text);
        return;
    }
    if (!m_source) {
        m_measureLabel->SetLabel(wxEmptyString);
        return;
    }
    const WaveViewState& state = m_view->State();
    m_measureLabel->SetLabel(
        "Loaded " + std::to_string(m_source->Signals().size()) +
        " signal(s), view " + std::to_string(state.timeOffset) +
        ".." + std::to_string(state.EndTime()));
}

void TraceViewPanel::HighlightNearestEvent()
{
    if (!m_view->HasPlayhead() || m_view->Events().empty()) return;
    const sigflow::trace::TimeValue playhead = m_view->Playhead();
    const auto& events = m_view->Events();
    std::size_t best = 0;
    sigflow::trace::TimeValue bestDelta = 0;
    bool haveBest = false;
    for (std::size_t i = 0; i < events.size(); ++i) {
        const sigflow::trace::TimeValue delta =
            (events[i].time > playhead) ? (events[i].time - playhead)
                                        : (playhead - events[i].time);
        if (!haveBest || delta < bestDelta) {
            best = i;
            bestDelta = delta;
            haveBest = true;
        }
    }
    if (haveBest && best < static_cast<std::size_t>(m_eventList->GetItemCount())) {
        m_eventList->SetItemState(static_cast<long>(best),
                                  wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED,
                                  wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED);
    }
}

bool TraceViewPanel::ParsePattern(const std::string& text, WavePatternSpec& spec) const
{
    spec.criteria.clear();
    if (!m_source) return false;
    std::istringstream stream(text);
    std::string token;
    while (std::getline(stream, token, ',')) {
        std::istringstream parts(token);
        std::string name, kind, value;
        std::getline(parts, name, ':');
        std::getline(parts, kind, ':');
        std::getline(parts, value, ':');
        if (name.empty()) return false;
        int signalId = -1;
        if (name == "*") {
            if (!m_view->State().visibleSignalIds.empty()) {
                signalId = m_view->State().visibleSignalIds.front();
            }
        } else {
            for (const sigflow::trace::SignalInfo& signal : m_source->Signals()) {
                if (signal.name == name) {
                    signalId = signal.id;
                    break;
                }
            }
        }
        if (signalId < 0) return false;
        WavePatternCriterion criterion;
        criterion.signalId = signalId;
        criterion.value = value;
        if (kind == "rise") criterion.kind = WavePatternKind::RisingEdge;
        else if (kind == "fall") criterion.kind = WavePatternKind::FallingEdge;
        else if (kind == "any") criterion.kind = WavePatternKind::AnyEdge;
        else if (kind == "high" || kind == "1") criterion.kind = WavePatternKind::High;
        else if (kind == "low" || kind == "0") criterion.kind = WavePatternKind::Low;
        else criterion.kind = WavePatternKind::Value;
        spec.criteria.push_back(criterion);
    }
    return !spec.criteria.empty();
}

void TraceViewPanel::RunPatternSearch(bool forward)
{
    if (!m_source || !m_view) return;
    WavePatternSpec spec;
    if (!ParsePattern(m_patternBox->GetValue().ToStdString(), spec)) {
        wxMessageBox("Pattern format: name:kind[:value], separated by comma.\n"
                     "kind: rise / fall / any / high / low / value",
                     "Pattern Search", wxOK | wxICON_INFORMATION, this);
        return;
    }
    const WaveViewState state = m_view->State();
    sigflow::trace::TimeValue from = 0;
    sigflow::trace::TimeValue to = state.maxTime;
    if (forward) {
        from = state.timeOffset;
        to = state.EndTime() + state.timeSpan;
    } else {
        from = 0;
        to = state.timeOffset;
    }
    sigflow::trace::TimeValue found = 0;
    std::string error;
    if (WavePatternFind(*m_source, spec, from, to, forward, 1, found, error)) {
        m_view->JumpToTime(found);
        m_view->SetPlayhead(found);
    } else {
        wxMessageBox("No match in search range.", "Pattern Search",
                     wxOK | wxICON_INFORMATION, this);
    }
}

void TraceViewPanel::OnOpenTrace(wxCommandEvent&)
{
    wxFileDialog dialog(this, "Open Trace", m_sessionDir, wxEmptyString,
                        "Trace files (*.vcd;*.fst)|*.vcd;*.fst|All files (*.*)|*.*",
                        wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    if (dialog.ShowModal() != wxID_OK) return;
    OpenTrace(dialog.GetPath().ToStdString());
}

void TraceViewPanel::OnAutoLoadTrace(wxCommandEvent&)
{
    AutoLoadTrace();
}

void TraceViewPanel::AutoLoadTrace()
{
    if (m_projectPath.empty()) {
        wxMessageBox("No project loaded.", "Auto Load", wxOK | wxICON_WARNING, this);
        return;
    }

    const wxString simRoot = wxString::FromUTF8(m_projectPath) +
                             wxFileName::GetPathSeparator() + ".sigflow" +
                             wxFileName::GetPathSeparator() + "sim";
    if (!wxDirExists(simRoot)) {
        wxMessageBox("No .sigflow/sim directory was found. Run simulation first.",
                     "Auto Load", wxOK | wxICON_INFORMATION, this);
        return;
    }

    std::vector<std::pair<wxString, wxString>> candidates;
    wxDir simDir(simRoot);
    wxString module;
    bool hasModule = simDir.GetFirst(&module, wxEmptyString, wxDIR_DIRS);
    while (hasModule) {
        wxFileName vcd(simRoot, wxEmptyString);
        vcd.AppendDir(module);
        vcd.AppendDir("waveform");
        vcd.SetFullName("wave.vcd");
        if (vcd.FileExists()) {
            candidates.emplace_back(module, vcd.GetFullPath());
        }
        hasModule = simDir.GetNext(&module);
    }

    if (candidates.empty()) {
        wxMessageBox("No .sigflow/sim/<module>/waveform/wave.vcd was found.",
                     "Auto Load", wxOK | wxICON_INFORMATION, this);
        return;
    }

    wxString selectedPath;
    if (candidates.size() == 1) {
        selectedPath = candidates.front().second;
    } else {
        wxArrayString choices;
        for (const auto& candidate : candidates) choices.Add(candidate.first);
        const int selected = wxGetSingleChoiceIndex(
            "Select a simulation waveform to load.", "Auto Load", choices, this);
        if (selected < 0) return;
        selectedPath = candidates[static_cast<std::size_t>(selected)].second;
    }
    OpenTrace(selectedPath.ToStdString());
}

void TraceViewPanel::OnSaveSession(wxCommandEvent&)
{
    wxFileDialog dialog(this, "Save Session", m_sessionDir, "wave.bws",
                        "Wave session (*.bws)|*.bws", wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
    if (dialog.ShowModal() != wxID_OK) return;
    SaveSession(dialog.GetPath().ToStdString());
}

void TraceViewPanel::OnLoadSession(wxCommandEvent&)
{
    wxFileDialog dialog(this, "Load Session", m_sessionDir, wxEmptyString,
                        "Wave session (*.bws)|*.bws", wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    if (dialog.ShowModal() != wxID_OK) return;
    LoadSession(dialog.GetPath().ToStdString());
}

void TraceViewPanel::OnTreeItemChecked(wxTreeListEvent& event)
{
    const auto* data = dynamic_cast<SignalTreeData*>(m_tree->GetItemData(event.GetItem()));
    if (data) {
        const bool checked = m_tree->GetCheckedState(event.GetItem()) == wxCHK_CHECKED;
        SetSignalChecked(data->signalId, checked);
    }
}

void TraceViewPanel::OnTreeItemActivated(wxTreeListEvent& event)
{
    const auto* data = dynamic_cast<SignalTreeData*>(m_tree->GetItemData(event.GetItem()));
    if (data) SetSignalChecked(data->signalId, true);
}

bool TraceViewPanel::AddSignalByName(const std::string& name)
{
    if (!m_source || name.empty()) return false;
    const std::size_t separator = name.find_last_of("./");
    const std::string leaf = separator == std::string::npos ? name : name.substr(separator + 1);
    for (const auto& signal : m_source->Signals()) {
        if (signal.name == name || signal.fullName == name || signal.name == leaf ||
            signal.fullName == leaf) {
            SetSignalChecked(signal.id, true);
            PopulateTree();
            return true;
        }
    }
    return false;
}

void TraceViewPanel::OnTreeContextMenu(wxTreeListEvent& event)
{
    const auto* data = dynamic_cast<SignalTreeData*>(m_tree->GetItemData(event.GetItem()));
    if (!data || !m_source) return;
    const int signalId = data->signalId;
    const auto* signal = m_source->SignalById(signalId);
    if (!signal) return;

    wxMenu menu;
    const int add = wxID_HIGHEST + 301;
    const int remove = wxID_HIGHEST + 302;
    const int alias = wxID_HIGHEST + 303;
    const int comment = wxID_HIGHEST + 304;
    menu.Append(add, "Add to waveform");
    menu.Append(remove, "Remove from waveform");
    menu.AppendSeparator();
    menu.Append(alias, "Set signal alias...");
    menu.Append(comment, "Set signal comment...");
    menu.Bind(wxEVT_MENU, [this, signalId](wxCommandEvent&) {
        SetSignalChecked(signalId, true);
    }, add);
    menu.Bind(wxEVT_MENU, [this, signalId](wxCommandEvent&) {
        SetSignalChecked(signalId, false);
    }, remove);
    menu.Bind(wxEVT_MENU, [this, signalId, signal](wxCommandEvent&) {
        const auto it = m_signalAliases.find(signalId);
        const wxString current = it == m_signalAliases.end()
            ? wxString() : wxString::FromUTF8(it->second);
        wxTextEntryDialog dialog(this, "Alias (empty clears it):", "Signal Alias", current);
        if (dialog.ShowModal() != wxID_OK) return;
        const std::string value = dialog.GetValue().ToStdString();
        if (value.empty()) m_signalAliases.erase(signalId);
        else m_signalAliases[signalId] = value;
        PopulateTree();
    }, alias);
    menu.Bind(wxEVT_MENU, [this, signalId, signal](wxCommandEvent&) {
        const auto it = m_signalComments.find(signalId);
        const wxString current = it == m_signalComments.end()
            ? wxString() : wxString::FromUTF8(it->second);
        wxTextEntryDialog dialog(this, "Comment (empty clears it):", "Signal Comment", current);
        if (dialog.ShowModal() != wxID_OK) return;
        const std::string value = dialog.GetValue().ToStdString();
        if (value.empty()) m_signalComments.erase(signalId);
        else m_signalComments[signalId] = value;
        wxMessageBox(value.empty() ? "Comment cleared." : value,
                     signal->fullName, wxOK | wxICON_INFORMATION, this);
    }, comment);
    m_tree->PopupMenu(&menu);
}

void TraceViewPanel::OnSearchChanged(wxCommandEvent&)
{
    PopulateTree();
}

void TraceViewPanel::OnEventActivated(wxListEvent& event)
{
    const long index = event.GetIndex();
    const auto& events = m_view->Events();
    if (index >= 0 && static_cast<std::size_t>(index) < events.size()) {
        m_view->JumpToTime(events[static_cast<std::size_t>(index)].time);
        m_view->SetPlayhead(events[static_cast<std::size_t>(index)].time);
        const WaveEvent& selected = events[static_cast<std::size_t>(index)];
        if (m_navigationCallback && !selected.signalName.empty()) {
            m_navigationCallback(selected.signalName, selected.sourcePath,
                                 selected.sourceLine, selected.time);
        }
    }
}

void TraceViewPanel::SaveSession(const std::string& path)
{
    WaveSessionData session = m_view->CaptureSession();
    session.uartCapturePath = m_uartCapturePath;
    session.signalAliases = m_signalAliases;
    session.signalComments = m_signalComments;
    std::string error;
    if (!WaveSessionSave(session, path, error)) {
        wxMessageBox(error, "Save Session", wxOK | wxICON_ERROR, this);
    }
}

bool TraceViewPanel::LoadSession(const std::string& path)
{
    WaveSessionData session;
    std::string error;
    if (!WaveSessionLoad(path, session, error)) {
        wxMessageBox(error, "Load Session", wxOK | wxICON_ERROR, this);
        return false;
    }
    if (!session.sourcePath.empty() && (!m_source || m_source->Path() != session.sourcePath)) {
        if (!OpenTrace(session.sourcePath)) return false;
    }
    m_view->ApplySession(session);
    if (!session.uartCapturePath.empty()) LoadUartCapture(session.uartCapturePath);
    if (m_themeButton) {
        m_themeButton->SetLabel(m_view->Theme() == WaveTheme::Dark
                                    ? "Theme: Dark" : "Theme: Light");
    }
    m_signalAliases = std::move(session.signalAliases);
    m_signalComments = std::move(session.signalComments);
    PopulateTree();
    RefreshEvents();
    RefreshMeasurement();
    return true;
}

} // namespace wave
} // namespace sigflow
