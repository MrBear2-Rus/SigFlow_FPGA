#pragma once

#include "agent/gui/AgentViewModel.h"
#include "ui/UiTheme.h"
#include <wx/panel.h>
#include <functional>
#include <vector>

class wxTextCtrl;
namespace agent::gui::support {

struct LocationTarget
{
    std::string ref, cardId, projectId, revision;
    vm::Origin origin = vm::Origin::Unknown;
    bool eligible = false;
    bool operator==(const LocationTarget&) const = default;
};
struct Block
{
    std::string title, text;
    int location = -1;
    bool operator==(const Block&) const = default;
};
struct Projection
{
    std::string issueSummary, evidenceSummary;
    std::vector<Block> issues, evidence;
    std::vector<LocationTarget> locations;
    bool operator==(const Projection&) const = default;
};

// Read-only projection. No file parsing, no source navigation, no backend calls.
Projection Project(const vm::AgentViewModel& model, const std::string& currentRevision = {});

class AgentSupportSection final : public wxPanel
{
public:
    AgentSupportSection(wxWindow* parent, bool evidence);
    bool SetProjection(const Projection& data);
    int Arrange(int width);
    wxString TextContent() const;
    // T25 supplies a trusted resolver. Default: no handler, all locate actions disabled.
    void SetLocateHandler(std::function<bool(const LocationTarget&)> handler);
    bool RequestLocation(size_t index);
    size_t RebuildCount() const { return m_rebuilds; }
private:
    void Rebuild();
    void ApplyTheme();
    Projection m_data;
    bool m_evidence = false, m_arranging = false, m_loaded = false;
    int m_width = 0;
    size_t m_rebuilds = 0;
    struct Item { wxTextCtrl* text; bool heading; wxWindow* action = nullptr; };
    std::vector<Item> m_items;
    std::function<bool(const LocationTarget&)> m_locate;
    UiTheme::Subscription m_theme;
};
} // namespace agent::gui::support
