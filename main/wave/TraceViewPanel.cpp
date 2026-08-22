#include "TraceViewPanel.h"

#include "WaveAnalysis.h"
#include "WavePatternSearch.h"
#include "WaveSession.h"
#include "../trace/TraceCache.h"
#include "../trace/VcdLazyTraceSource.h"

#include <wx/button.h>
#include <wx/filedlg.h>
#include <wx/menu.h>
#include <wx/msgdlg.h>
#include <wx/sizer.h>
#include <wx/splitter.h>

#include <algorithm>
#include <sstream>

namespace sigflow {
namespace wave {

TraceViewPanel::TraceViewPanel(wxWindow* parent)
    : wxPanel(parent, wxID_ANY)
{
    BuildUi();
}

void TraceViewPanel::BuildUi()
{
    wxBoxSizer* root = new wxBoxSizer(wxVERTICAL);

    // 工具栏
    wxBoxSizer* toolbar = new wxBoxSizer(wxHORIZONTAL);
    wxButton* open = new wxButton(this, wxID_ANY, "Open VCD");
    wxButton* save = new wxButton(this, wxID_ANY, "Save Session");
    wxButton* load = new wxButton(this, wxID_ANY, "Load Session");
    wxButton* zoomIn = new wxButton(this, wxID_ANY, "Zoom+");
    wxButton* zoomOut = new wxButton(this, wxID_ANY, "Zoom-");
    wxButton* zoomReset = new wxButton(this, wxID_ANY, "Fit");
    wxButton* clearMarkers = new wxButton(this, wxID_ANY, "Clear Markers");
    wxButton* clearAB = new wxButton(this, wxID_ANY, "Clear A-B");
    for (wxButton* button : { open, save, load, zoomIn, zoomOut, zoomReset,
                              clearMarkers, clearAB }) {
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
    m_tree = new wxTreeCtrl(left, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                            wxTR_HIDE_ROOT | wxTR_DEFAULT_STYLE);
    leftLayout->Add(m_tree, 1, wxEXPAND | wxALL, 2);
    left->SetSizer(leftLayout);
    left->SetMinSize(wxSize(220, -1));

    m_view = new WaveformView(splitter);
    splitter->SplitVertically(left, m_view, 220);
    splitter->SetMinimumPaneSize(160);
    root->Add(splitter, 1, wxEXPAND);

    // 底部：事件列表 + 模式搜索
    wxBoxSizer* bottom = new wxBoxSizer(wxHORIZONTAL);
    m_eventList = new wxListCtrl(this, wxID_ANY, wxDefaultPosition, wxSize(360, 110),
                                 wxLC_REPORT | wxLC_SINGLE_SEL | wxBORDER_SIMPLE);
    m_eventList->AppendColumn("Time", wxLIST_FORMAT_LEFT, 90);
    m_eventList->AppendColumn("Event", wxLIST_FORMAT_LEFT, 240);
    bottom->Add(m_eventList, 0, wxALL, 2);

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
    save->Bind(wxEVT_BUTTON, &TraceViewPanel::OnSaveSession, this);
    load->Bind(wxEVT_BUTTON, &TraceViewPanel::OnLoadSession, this);
    zoomIn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { m_view->ZoomIn(); });
    zoomOut->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { m_view->ZoomOut(); });
    zoomReset->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { m_view->ZoomReset(); });
    clearMarkers->Bind(wxEVT_BUTTON,
                       [this](wxCommandEvent&) { m_view->ClearMarkers(); });
    clearAB->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { m_view->ClearAB(); });
    m_tree->Bind(wxEVT_TREE_ITEM_ACTIVATED, &TraceViewPanel::OnTreeActivated, this);
    m_searchBox->Bind(wxEVT_TEXT, &TraceViewPanel::OnSearchChanged, this);
    m_eventList->Bind(wxEVT_LIST_ITEM_ACTIVATED, &TraceViewPanel::OnEventActivated, this);
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
    PopulateTree();
    RefreshEvents();
    RefreshMeasurement();
    return true;
}

void TraceViewPanel::PopulateTree()
{
    m_tree->DeleteAllItems();
    const wxTreeItemId root = m_tree->AddRoot("Signals");
    if (!m_source) return;

    const std::string filter = m_searchBox->GetValue().Lower().ToStdString();
    for (const sigflow::trace::SignalInfo& signal : m_source->Signals()) {
        if (!filter.empty() &&
            signal.name.find(filter) == std::string::npos &&
            signal.fullName.find(filter) == std::string::npos) {
            continue;
        }
        // 按 scope 分段建模块节点
        wxTreeItemId parent = root;
        std::string scope = signal.scope;
        while (!scope.empty()) {
            const std::size_t slash = scope.find('/');
            const std::string part = scope.substr(0, slash);
            wxTreeItemIdValue cookie = nullptr;
            wxTreeItemId child = m_tree->GetFirstChild(parent, cookie);
            wxTreeItemId match;
            while (child.IsOk()) {
                if (m_tree->GetItemText(child) == part) {
                    match = child;
                    break;
                }
                child = m_tree->GetNextSibling(child);
            }
            if (!match.IsOk()) {
                match = m_tree->AppendItem(parent, part);
            }
            parent = match;
            scope = (slash == std::string::npos) ? std::string() : scope.substr(slash + 1);
        }
        wxTreeItemId leaf = m_tree->AppendItem(parent, signal.name);
        m_tree->SetItemData(leaf, new SignalTreeData(signal.id));
    }
    m_tree->ExpandAllChildren(root);
}

void TraceViewPanel::ToggleSignal(int signalId)
{
    if (!m_view) return;
    WaveViewState state = m_view->State();
    const auto it = std::find(state.visibleSignalIds.begin(),
                              state.visibleSignalIds.end(), signalId);
    if (it != state.visibleSignalIds.end()) {
        state.visibleSignalIds.erase(it);
    } else {
        state.visibleSignalIds.push_back(signalId);
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
    m_measureLabel->SetLabel(text);
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

void TraceViewPanel::OnTreeActivated(wxTreeEvent& event)
{
    const auto* data = dynamic_cast<SignalTreeData*>(m_tree->GetItemData(event.GetItem()));
    if (data) ToggleSignal(data->signalId);
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
    }
}

void TraceViewPanel::SaveSession(const std::string& path)
{
    WaveSessionData session = m_view->CaptureSession();
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
    PopulateTree();
    RefreshEvents();
    RefreshMeasurement();
    return true;
}

} // namespace wave
} // namespace sigflow
