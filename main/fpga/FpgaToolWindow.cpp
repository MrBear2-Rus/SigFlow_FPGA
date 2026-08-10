#include "FpgaToolWindow.h"

#include <utility>

#include <wx/button.h>
#include <wx/dir.h>
#include <wx/filedlg.h>
#include <wx/filefn.h>
#include <wx/notebook.h>
#include <wx/panel.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>

#include "CstValidator.h"
#include "FpgaSynthesisJobsPanel.h"
#include "NextpnrJobsPanel.h"
#include "FpgaTheme.h"
#include "../FpgaYosysRuntime.h"

namespace {

wxStaticText* AddInfoRow(wxWindow* parent, wxBoxSizer* layout, const wxString& label,
                         const wxString& value, const wxColour& valueColour)
{
    wxBoxSizer* row = new wxBoxSizer(wxHORIZONTAL);
    row->Add(FpgaTheme::MakeLabel(parent, label, FpgaTheme::kTextMuted),
             0, wxALIGN_CENTER_VERTICAL | wxRIGHT, parent->FromDIP(10));
    row->AddStretchSpacer();
    wxStaticText* valueLabel = FpgaTheme::MakeLabel(parent, value, valueColour);
    row->Add(valueLabel, 0, wxALIGN_CENTER_VERTICAL);
    layout->Add(row, 0, wxEXPAND | wxTOP | wxBOTTOM, parent->FromDIP(3));
    return valueLabel;
}

wxPanel* MakeInfoCard(wxWindow* parent, wxBoxSizer* layout, const wxString& title)
{
    wxPanel* card = new wxPanel(parent);
    card->SetBackgroundColour(FpgaTheme::kPanel);
    wxBoxSizer* cardLayout = new wxBoxSizer(wxVERTICAL);
    wxStaticText* heading = FpgaTheme::MakeLabel(card, title, FpgaTheme::kTextSecondary, true);
    heading->SetFont(FpgaTheme::SectionFont(card));
    cardLayout->Add(heading, 0, wxBOTTOM, card->FromDIP(6));
    layout->Add(card, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, parent->FromDIP(10));
    return card;
}

} // namespace

FpgaToolWindow::FpgaToolWindow(wxWindow* parent)
    : wxFrame(parent, wxID_ANY, "SigFlow FPGA Tools", wxDefaultPosition, wxSize(960, 700),
              wxDEFAULT_FRAME_STYLE | wxFRAME_FLOAT_ON_PARENT)
{
    BuildUi();
    Bind(wxEVT_CLOSE_WINDOW, &FpgaToolWindow::OnClose, this);
}

void FpgaToolWindow::BuildUi()
{
    SetBackgroundColour(FpgaTheme::kBackground);

    wxBoxSizer* layout = new wxBoxSizer(wxVERTICAL);
    layout->Add(BuildHeader(), 0, wxEXPAND);

    m_notebook = new wxNotebook(this, wxID_ANY);
    m_notebook->SetBackgroundColour(FpgaTheme::kBackground);
    m_notebook->AddPage(BuildYosysPage(), "Yosys");
    m_notebook->AddPage(BuildNextpnrPage(), "nextpnr");
    m_notebook->AddPage(BuildProgrammerPage(), "openFPGALoader");
    layout->Add(m_notebook, 1, wxEXPAND);
    SetSizer(layout);
}

wxPanel* FpgaToolWindow::BuildHeader()
{
    wxPanel* header = new wxPanel(this);
    header->SetBackgroundColour(FpgaTheme::kHeader);
    header->SetMinSize(wxSize(-1, header->FromDIP(46)));

    wxBoxSizer* layout = new wxBoxSizer(wxHORIZONTAL);
    wxStaticText* title = FpgaTheme::MakeLabel(header, "SigFlow FPGA Tools",
                                               FpgaTheme::kText, true);
    title->SetFont(FpgaTheme::TitleFont(header));
    layout->Add(title, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, header->FromDIP(14));

    layout->AddStretchSpacer();

    wxStaticText* badge = new wxStaticText(header, wxID_ANY, " Tang Nano 9K ");
    badge->SetBackgroundColour(FpgaTheme::kPanelAlt);
    badge->SetForegroundColour(FpgaTheme::kBlue);
    wxFont badgeFont = badge->GetFont();
    badgeFont.MakeBold();
    badge->SetFont(badgeFont);
    layout->Add(badge, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, header->FromDIP(10));

    m_headerProjectLabel = FpgaTheme::MakeLabel(header, "Project: no project open",
                                                FpgaTheme::kTextSecondary);
    layout->Add(m_headerProjectLabel, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT,
                header->FromDIP(10));

    header->SetSizer(layout);
    return header;
}

wxPanel* FpgaToolWindow::BuildYosysPage()
{
    wxPanel* page = new wxPanel(m_notebook);
    page->SetBackgroundColour(FpgaTheme::kBackground);
    wxBoxSizer* layout = new wxBoxSizer(wxVERTICAL);

    wxBoxSizer* header = new wxBoxSizer(wxHORIZONTAL);
    wxBoxSizer* titleColumn = new wxBoxSizer(wxVERTICAL);
    wxStaticText* title = FpgaTheme::MakeLabel(page, "Yosys Synthesis", FpgaTheme::kText, true);
    title->SetFont(FpgaTheme::TitleFont(page));
    titleColumn->Add(title, 0, wxBOTTOM, page->FromDIP(2));
    titleColumn->Add(FpgaTheme::MakeLabel(page,
        wxT("综合 RTL 并生成经过校验的 Yosys JSON 网表。"),
        FpgaTheme::kTextSecondary));
    header->Add(titleColumn, 1, wxALIGN_CENTER_VERTICAL);

    wxButton* start = new wxButton(page, wxID_ANY, "Start Synthesis");
    FpgaTheme::StyleButton(start, FpgaTheme::kGreen, *wxBLACK);
    wxButton* cancel = new wxButton(page, wxID_ANY, "Cancel");
    FpgaTheme::StyleButton(cancel, FpgaTheme::kRed, *wxWHITE);
    wxButton* refresh = new wxButton(page, wxID_REFRESH, "Refresh");
    FpgaTheme::StyleSecondaryButton(refresh);
    header->Add(start, 0, wxLEFT, page->FromDIP(6));
    header->Add(cancel, 0, wxLEFT, page->FromDIP(6));
    header->Add(refresh, 0, wxLEFT, page->FromDIP(6));
    layout->Add(header, 0, wxEXPAND | wxALL, page->FromDIP(10));

    FpgaTheme::MakeDivider(page, layout, page->FromDIP(10));

    m_synthesisJobsPanel = new FpgaSynthesisJobsPanel(page);
    layout->Add(m_synthesisJobsPanel, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM,
                page->FromDIP(10));
    page->SetSizer(layout);

    start->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (m_synthesisStartHandler) m_synthesisStartHandler();
    });
    cancel->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (m_synthesisCancelHandler) m_synthesisCancelHandler();
    });
    refresh->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { RefreshSynthesisJobs(); });
    return page;
}

wxPanel* FpgaToolWindow::BuildNextpnrPage()
{
    wxPanel* page = new wxPanel(m_notebook);
    page->SetBackgroundColour(FpgaTheme::kBackground);
    wxBoxSizer* layout = new wxBoxSizer(wxVERTICAL);

    wxBoxSizer* header = new wxBoxSizer(wxHORIZONTAL);
    wxBoxSizer* titleColumn = new wxBoxSizer(wxVERTICAL);
    wxStaticText* title = FpgaTheme::MakeLabel(page, "Place & Route", FpgaTheme::kText, true);
    title->SetFont(FpgaTheme::TitleFont(page));
    titleColumn->Add(title, 0, wxBOTTOM, page->FromDIP(2));
    titleColumn->Add(FpgaTheme::MakeLabel(page,
        wxT("使用 Yosys 网表与 CST 约束执行布局布线。"),
        FpgaTheme::kTextSecondary));
    header->Add(titleColumn, 1, wxALIGN_CENTER_VERTICAL);

    m_routeStartButton = new wxButton(page, wxID_ANY, "Start Place and Route");
    FpgaTheme::StyleButton(m_routeStartButton, FpgaTheme::kBlue, *wxWHITE);
    header->Add(m_routeStartButton, 0, wxLEFT, page->FromDIP(6));

    wxButton* routeCancelButton = new wxButton(page, wxID_ANY, "Cancel");
    FpgaTheme::StyleButton(routeCancelButton, FpgaTheme::kRed, *wxWHITE);
    header->Add(routeCancelButton, 0, wxLEFT, page->FromDIP(6));
    layout->Add(header, 0, wxEXPAND | wxALL, page->FromDIP(10));

    FpgaTheme::MakeDivider(page, layout, page->FromDIP(10));

    // ── 任务列表面板 ──
    m_routeJobsPanel = new NextpnrJobsPanel(page);
    layout->Add(m_routeJobsPanel, 1, wxEXPAND | wxLEFT | wxRIGHT, page->FromDIP(8));

    FpgaTheme::MakeDivider(page, layout, page->FromDIP(6));

    // ── 本次运行信息卡片 ──
    wxPanel* infoCard = MakeInfoCard(page, layout, "Run Summary");
    wxBoxSizer* infoLayout = new wxBoxSizer(wxVERTICAL);
    m_nextpnrDeviceLabel = AddInfoRow(infoCard, infoLayout, "Device", "", FpgaTheme::kText);
    m_nextpnrFamilyLabel = AddInfoRow(infoCard, infoLayout, "Family", "", FpgaTheme::kText);
    m_nextpnrJsonLabel = AddInfoRow(infoCard, infoLayout, "Netlist JSON", "", FpgaTheme::kText);
    m_nextpnrCstLabel = AddInfoRow(infoCard, infoLayout, "Constraints (CST)", "",
                                   FpgaTheme::kText);
    infoCard->SetSizer(infoLayout);

    // ── 命令预览卡片 ──
    wxPanel* commandCard = MakeInfoCard(page, layout, "Command");
    wxBoxSizer* commandLayout = new wxBoxSizer(wxVERTICAL);
    wxTextCtrl* commandPreview = new wxTextCtrl(commandCard, wxID_ANY, wxEmptyString,
        wxDefaultPosition, wxSize(-1, FromDIP(64)),
        wxTE_MULTILINE | wxTE_READONLY);
    commandPreview->SetBackgroundColour(FpgaTheme::kPanelAlt);
    commandPreview->SetForegroundColour(FpgaTheme::kTextSecondary);
    commandPreview->SetValue(
        "nextpnr-himbaechel --json <project>\\yosys\\<top>.json "
        "--write <project>\\nextpnr\\<top>.pnr.json "
        "--device GW1NR-LV9QN88PC6/I5 --vopt family=GW1N-9C");
    commandLayout->Add(commandPreview, 1, wxEXPAND);
    commandLayout->Add(FpgaTheme::MakeLabel(commandCard,
        wxT("可在 sigflow.project 的 fpga.nextpnr_args 中覆盖参数；运行后可继续用 ")
        wxT("gowin_pack -d GW1N-9C 打包 .fs 位流。"),
        FpgaTheme::kTextMuted), 0, wxTOP, commandCard->FromDIP(6));
    commandCard->SetSizer(commandLayout);

    layout->AddStretchSpacer(1);
    page->SetSizer(layout);

    m_routeStartButton->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (m_routeStartHandler) m_routeStartHandler();
    });
    routeCancelButton->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (m_routeCancelHandler) m_routeCancelHandler();
    });
    return page;
}

wxPanel* FpgaToolWindow::BuildProgrammerPage()
{
    wxPanel* page = new wxPanel(m_notebook);
    page->SetBackgroundColour(FpgaTheme::kBackground);
    wxBoxSizer* layout = new wxBoxSizer(wxVERTICAL);

    wxBoxSizer* header = new wxBoxSizer(wxHORIZONTAL);
    wxBoxSizer* titleColumn = new wxBoxSizer(wxVERTICAL);
    wxStaticText* title = FpgaTheme::MakeLabel(page, "Program Board", FpgaTheme::kText, true);
    title->SetFont(FpgaTheme::TitleFont(page));
    titleColumn->Add(title, 0, wxBOTTOM, page->FromDIP(2));
    titleColumn->Add(FpgaTheme::MakeLabel(page,
        wxT("选择 Apicula .fs 位流并下载到目标板卡。"),
        FpgaTheme::kTextSecondary));
    header->Add(titleColumn, 1, wxALIGN_CENTER_VERTICAL);

    m_programButton = new wxButton(page, wxID_ANY, "Program Board");
    FpgaTheme::StyleButton(m_programButton, FpgaTheme::kAmber, *wxBLACK);
    m_programButton->Enable(false);
    header->Add(m_programButton, 0, wxLEFT, page->FromDIP(6));
    layout->Add(header, 0, wxEXPAND | wxALL, page->FromDIP(10));

    FpgaTheme::MakeDivider(page, layout, page->FromDIP(10));

    wxPanel* infoCard = MakeInfoCard(page, layout, "Target");
    wxBoxSizer* infoLayout = new wxBoxSizer(wxVERTICAL);
    m_programBoardLabel = AddInfoRow(infoCard, infoLayout, "Board", "", FpgaTheme::kText);
    AddInfoRow(infoCard, infoLayout, "Programmer", "openFPGALoader",
               FpgaTheme::kTextSecondary);
    infoCard->SetSizer(infoLayout);

    // ── 位流选择 ──
    wxBoxSizer* picker = new wxBoxSizer(wxHORIZONTAL);
    m_bitstreamPathText = new wxTextCtrl(page, wxID_ANY, wxEmptyString, wxDefaultPosition,
        wxDefaultSize, wxTE_READONLY);
    m_bitstreamPathText->SetBackgroundColour(FpgaTheme::kPanelAlt);
    m_bitstreamPathText->SetForegroundColour(FpgaTheme::kText);
    wxButton* browse = new wxButton(page, wxID_OPEN, "Select .fs");
    FpgaTheme::StyleSecondaryButton(browse);
    picker->Add(m_bitstreamPathText, 1, wxRIGHT, page->FromDIP(6));
    picker->Add(browse, 0);
    layout->Add(picker, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, page->FromDIP(10));

    m_programStatusLabel = FpgaTheme::MakeLabel(page,
        wxT("选择 .fs 位流后即可烧录。"), FpgaTheme::kTextMuted);
    layout->Add(m_programStatusLabel, 0, wxLEFT | wxRIGHT | wxBOTTOM, page->FromDIP(10));

    layout->AddStretchSpacer(1);
    page->SetSizer(layout);

    browse->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        wxFileDialog dialog(this, "Select an Apicula bitstream", m_projectPath, wxEmptyString,
            "Gowin bitstream (*.fs)|*.fs|All files (*.*)|*.*", wxFD_OPEN | wxFD_FILE_MUST_EXIST);
        if (dialog.ShowModal() != wxID_OK) return;
        m_bitstreamPath = dialog.GetPath();
        m_bitstreamPathText->SetValue(m_bitstreamPath);
        m_programStatusLabel->SetLabel("Ready to program: " + m_bitstreamPath);
        m_programButton->Enable(static_cast<bool>(m_programStartHandler));
    });
    m_programButton->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (m_programStartHandler && !m_bitstreamPath.IsEmpty()) {
            m_programStatusLabel->SetLabel("Programming started...");
            m_programStartHandler(m_bitstreamPath);
        }
    });
    return page;
}

void FpgaToolWindow::ShowPage(FpgaToolPage page)
{
    const int selection = page == FpgaToolPage::Yosys ? 0 :
                          page == FpgaToolPage::Nextpnr ? 1 : 2;
    m_notebook->SetSelection(selection);
    Show();
    Raise();
    SetFocus();
    if (page == FpgaToolPage::Yosys) RefreshSynthesisJobs();
}

void FpgaToolWindow::SetProjectContext(const wxString& projectPath,
                                       const wxString& activeYosysJobId)
{
    m_projectPath = projectPath;
    m_activeYosysJobId = activeYosysJobId;
    m_synthesisJobsPanel->SetProjectContext(projectPath, activeYosysJobId);
    if (m_routeJobsPanel) m_routeJobsPanel->SetProjectContext(projectPath, wxEmptyString);
    UpdateProjectLabels();
}

void FpgaToolWindow::RefreshSynthesisJobs()
{
    m_synthesisJobsPanel->SetProjectContext(m_projectPath, m_activeYosysJobId);
    m_synthesisJobsPanel->RefreshJobs();
}

void FpgaToolWindow::SetOpenFileHandler(std::function<void(const wxString&, long)> handler)
{
    m_synthesisJobsPanel->SetOpenFileHandler(handler);
    if (m_routeJobsPanel) m_routeJobsPanel->SetOpenFileHandler(handler);
}

void FpgaToolWindow::SetSynthesisStartHandler(std::function<void()> handler)
{
    m_synthesisStartHandler = std::move(handler);
}

void FpgaToolWindow::SetSynthesisCancelHandler(std::function<void()> handler)
{
    m_synthesisCancelHandler = std::move(handler);
}

void FpgaToolWindow::SetSynthesisRetryHandler(std::function<void(const wxString&)> handler)
{
    m_synthesisRetryHandler = std::move(handler);
    m_synthesisJobsPanel->SetRetryHandler(m_synthesisRetryHandler);
}

void FpgaToolWindow::SetRouteStartHandler(std::function<void()> handler)
{
    m_routeStartHandler = std::move(handler);
}

void FpgaToolWindow::SetRouteCancelHandler(std::function<void()> handler)
{
    m_routeCancelHandler = std::move(handler);
}

void FpgaToolWindow::SetRouteRetryHandler(std::function<void(const wxString&)> handler)
{
    m_routeRetryHandler = std::move(handler);
    if (m_routeJobsPanel) m_routeJobsPanel->SetRetryHandler(m_routeRetryHandler);
}

void FpgaToolWindow::SetRouteActiveJob(const wxString& jobId)
{
    m_activeNextpnrJobId = jobId;
    // 立即同步到面板，防止定时器误将 running job 当做僵尸进程回收
    if (m_routeJobsPanel) {
        m_routeJobsPanel->SetProjectContext(m_projectPath, m_activeNextpnrJobId);
    }
}

void FpgaToolWindow::RefreshRouteJobs()
{
    if (m_routeJobsPanel) {
        m_routeJobsPanel->SetProjectContext(m_projectPath, m_activeNextpnrJobId);
        m_routeJobsPanel->RefreshJobs();
    }
}

void FpgaToolWindow::SetProgramStartHandler(std::function<void(const wxString&)> handler)
{
    m_programStartHandler = std::move(handler);
    if (m_programButton && !m_bitstreamPath.IsEmpty()) m_programButton->Enable(true);
}

void FpgaToolWindow::UpdateProjectLabels()
{
    const wxString project = m_projectPath.IsEmpty() ? wxString("no project open") : m_projectPath;
    m_headerProjectLabel->SetLabel("Project: " + project);
    UpdateNextpnrInfo();
    UpdateProgrammerInfo();
}

void FpgaToolWindow::UpdateNextpnrInfo()
{
    const FpgaTargetProfile& profile = GetTangNano9kTargetProfile();
    m_nextpnrDeviceLabel->SetLabel(profile.device);
    m_nextpnrFamilyLabel->SetLabel(profile.family);

    wxString jsonText = wxT("未找到网表，请先运行 Synthesis");
    wxColour jsonColour = FpgaTheme::kRed;
    if (!m_projectPath.IsEmpty()) {
        const wxString yosysDirectory = m_projectPath + "\\yosys";
        int count = 0;
        if (wxDirExists(yosysDirectory)) {
            wxDir directory(yosysDirectory);
            wxString name;
            if (directory.GetFirst(&name, "*.json", wxDIR_FILES)) {
                do {
                    if (!name.Lower().Contains("manifest")) ++count;
                } while (directory.GetNext(&name));
            }
        }
        if (count > 0) {
            jsonText = wxString::Format(wxT("%d 个网表 (yosys\\*.json)"), count);
            jsonColour = FpgaTheme::kGreen;
        }
    }
    m_nextpnrJsonLabel->SetLabel(jsonText);
    m_nextpnrJsonLabel->SetForegroundColour(jsonColour);

    if (m_projectPath.IsEmpty()) {
        m_nextpnrCstLabel->SetLabel(wxT("打开项目后自动解析"));
        m_nextpnrCstLabel->SetForegroundColour(FpgaTheme::kTextSecondary);
        return;
    }
    const wxString cstPath = CstValidator::AutoResolveCst(m_projectPath, wxEmptyString);
    if (wxFileExists(cstPath)) {
        m_nextpnrCstLabel->SetLabel(cstPath);
        m_nextpnrCstLabel->SetForegroundColour(FpgaTheme::kGreen);
    } else {
        m_nextpnrCstLabel->SetLabel(wxT("未找到 CST（可选，可稍后通过 Pin Constraints 生成）"));
        m_nextpnrCstLabel->SetForegroundColour(FpgaTheme::kTextSecondary);
    }
}

void FpgaToolWindow::UpdateProgrammerInfo()
{
    const FpgaTargetProfile& profile = GetTangNano9kTargetProfile();
    m_programBoardLabel->SetLabel(profile.programmerBoard);
}

void FpgaToolWindow::OnClose(wxCloseEvent& event)
{
    if (event.CanVeto()) {
        Hide();
        return;
    }
    event.Skip();
}
