#include "FpgaPinBindingPanel.h"
#include "FpgaConstraint.h"
#include <wx/msgdlg.h>
#include <wx/datetime.h>
#include <wx/dcbuffer.h>
#include <wx/file.h>
#include <wx/filedlg.h>
#include <wx/filename.h>
#include <wx/sizer.h>
#include <wx/statline.h>
#include <wx/colour.h>
#include <algorithm>

// ==================== PackageView ====================

wxBEGIN_EVENT_TABLE(PackageView, wxWindow)
    EVT_PAINT(PackageView::OnPaint)
    EVT_LEFT_DOWN(PackageView::OnMouseDown)
    EVT_MOTION(PackageView::OnMouseMove)
wxEND_EVENT_TABLE()

PackageView::PackageView(wxWindow* parent, const FpgaPinDatabase& pinDb)
    : wxWindow(parent, wxID_ANY, wxDefaultPosition, wxSize(340, 340),
        wxFULL_REPAINT_ON_RESIZE | wxBORDER_SIMPLE),
      m_pinDb(pinDb)
{
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    SetMinSize(wxSize(300, 300));
}

wxRect PackageView::GetPinRect(int pinNumber) const {
    if (pinNumber < 1 || pinNumber > 88) return wxRect();
    wxSize sz = GetClientSize();
    int w = sz.GetWidth(), h = sz.GetHeight();
    int margin = 30;
    int chipW = w - 2 * margin;
    int chipH = h - 2 * margin;
    int pinSize = std::max(8, std::min(14, chipW / 28));

    // Top edge: pins 1-22
    if (pinNumber <= 22) {
        int x = margin + (pinNumber - 1) * chipW / 22;
        return wxRect(x - pinSize / 2, margin - pinSize, pinSize, pinSize);
    }
    // Right edge: pins 23-44
    if (pinNumber <= 44) {
        int y = margin + (pinNumber - 23) * chipH / 22;
        return wxRect(margin + chipW, y - pinSize / 2, pinSize, pinSize);
    }
    // Bottom edge: pins 45-66
    if (pinNumber <= 66) {
        int x = margin + (22 - (pinNumber - 44)) * chipW / 22;
        return wxRect(x - pinSize / 2, margin + chipH, pinSize, pinSize);
    }
    // Left edge: pins 67-88
    int y = margin + (22 - (pinNumber - 66)) * chipH / 22;
    return wxRect(margin - pinSize, y - pinSize / 2, pinSize, pinSize);
}

int PackageView::HitTestPin(const wxPoint& pt) const {
    for (int i = 1; i <= 88; ++i) {
        wxRect r = GetPinRect(i);
        if (r.Contains(pt)) return i;
    }
    return -1;
}

void PackageView::OnPaint(wxPaintEvent&) {
    wxAutoBufferedPaintDC dc(this);
    dc.SetBackground(wxBrush(wxColour(30, 30, 32)));
    dc.Clear();

    wxSize sz = GetClientSize();
    int margin = 30;
    int chipW = sz.GetWidth() - 2 * margin;
    int chipH = sz.GetHeight() - 2 * margin;

    // 芯片主体
    dc.SetPen(wxPen(wxColour(80, 80, 90), 2));
    dc.SetBrush(wxBrush(wxColour(45, 45, 50)));
    dc.DrawRoundedRectangle(margin, margin, chipW, chipH, 6);

    // 芯片标签
    dc.SetTextForeground(wxColour(180, 180, 190));
    wxFont font(7, wxFONTFAMILY_SWISS, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_BOLD);
    dc.SetFont(font);
    dc.DrawText("GW1NR-9", margin + 8, margin + chipH / 2 - 20);
    dc.DrawText("QFN88", margin + 8, margin + chipH / 2 - 8);

    // 绘制引脚
    for (int i = 1; i <= 88; ++i) {
        wxRect r = GetPinRect(i);
        const auto* pin = m_pinDb.FindPin(i);

        // 颜色: 可用 = 按Bank着色, 不可用 = 暗灰, 已绑定 = 绿色高亮, 选中 = 橙色边框
        wxColour fillColor;
        if (!pin || !pin->available) {
            fillColor = wxColour(60, 60, 65);
        } else if (m_highlightedPins.count(i)) {
            fillColor = wxColour(34, 197, 94);
        } else {
            switch (pin->bank) {
            case 0: fillColor = wxColour(59, 130, 246); break;
            case 1: fillColor = wxColour(168, 85, 247); break;
            default: fillColor = wxColour(234, 179, 8); break;
            }
        }

        dc.SetPen(wxPen(wxColour(0, 0, 0), 1));
        dc.SetBrush(wxBrush(fillColor));

        if (i == m_selectedPin || i == m_hoveredPin) {
            wxRect bigR = r; bigR.Inflate(2);
            dc.SetPen(wxPen(wxColour(251, 146, 60), 2));
            dc.DrawRoundedRectangle(bigR, 2);
        } else {
            dc.DrawRoundedRectangle(r, 2);
        }
    }
}

void PackageView::OnMouseDown(wxMouseEvent& evt) {
    int pin = HitTestPin(evt.GetPosition());
    m_selectedPin = pin;
    Refresh();
    if (pin > 0 && onPinClicked) {
        onPinClicked(pin);
    }
}

void PackageView::OnMouseMove(wxMouseEvent& evt) {
    int pin = HitTestPin(evt.GetPosition());
    if (pin != m_hoveredPin) {
        m_hoveredPin = pin;
        Refresh();
    }
}

void PackageView::SetSelectedPin(int pin) {
    m_selectedPin = pin;
    Refresh();
}

void PackageView::SetHighlightedPins(const std::set<int>& pins) {
    m_highlightedPins = pins;
    Refresh();
}

// ==================== PortListCtrl ====================

wxBEGIN_EVENT_TABLE(PortListCtrl, wxListCtrl)
    EVT_LIST_ITEM_SELECTED(wxID_ANY, PortListCtrl::OnItemSelected)
wxEND_EVENT_TABLE()

PortListCtrl::PortListCtrl(wxWindow* parent)
    : wxListCtrl(parent, wxID_ANY, wxDefaultPosition, wxSize(200, 250),
        wxLC_REPORT | wxLC_SINGLE_SEL | wxBORDER_SIMPLE)
{
    AppendColumn("Port", wxLIST_FORMAT_LEFT, 100);
    AppendColumn("Dir", wxLIST_FORMAT_LEFT, 50);
    AppendColumn("Width", wxLIST_FORMAT_LEFT, 45);
    AppendColumn("Pin", wxLIST_FORMAT_LEFT, 45);
}

void PortListCtrl::PopulatePorts(
    const FpgaPortMap& ports,
    const ConstraintSheet& sheet)
{
    Freeze();
    DeleteAllItems();

    // 转换为可排序的 vector
    std::vector<std::pair<wxString, std::pair<FpgaPortDirection, int>>> sortedPorts(
        ports.begin(), ports.end());
    std::sort(sortedPorts.begin(), sortedPorts.end());

    for (const auto& [name, info] : sortedPorts) {
        const auto& [dir, width] = info;
        long idx = InsertItem(GetItemCount(), name);
        SetItem(idx, 1, FpgaPortDirectionToString(dir));
        SetItem(idx, 2, wxString::Format("%d", width));

        // 检查是否已绑定
        const auto* binding = sheet.FindBinding(name, 0);
        if (binding && binding->packagePin > 0) {
            SetItem(idx, 3, wxString::Format("%d", binding->packagePin));
            SetItemTextColour(idx, wxColour(34, 197, 94));
        } else {
            // 未绑定 = 橙色
            SetItemTextColour(idx, wxColour(234, 179, 8));
        }
    }

    Thaw();
}

int PortListCtrl::GetSelectedPortBitIndex() const {
    long idx = GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
    return idx >= 0 ? 0 : -1;  // 简化: 标量端口默认 bit=0
}

void PortListCtrl::OnItemSelected(wxListEvent& evt) {
    evt.Skip();
}

// ==================== FpgaPinBindingPanel ====================

FpgaPinBindingPanel::FpgaPinBindingPanel(wxWindow* parent, MainFrame* mainFrame)
    : wxScrolledWindow(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize,
        wxHSCROLL | wxVSCROLL | wxBORDER_NONE),
      m_mainFrame(mainFrame),
      m_pinDb(GetQFN88PinDatabase()),
      m_validator(m_pinDb)
{
    SetScrollRate(10, 10);
    SetBackgroundColour(wxColour(30, 30, 32));
    BuildUI();
}

void FpgaPinBindingPanel::BuildUI() {
    m_mainSizer = new wxBoxSizer(wxVERTICAL);

    // --- 标题栏 ---
    wxBoxSizer* titleBar = new wxBoxSizer(wxHORIZONTAL);
    m_titleLabel = new wxStaticText(this, wxID_ANY, "FPGA Pin Binding",
        wxDefaultPosition, wxDefaultSize, wxALIGN_LEFT);
    wxFont titleFont(12, wxFONTFAMILY_SWISS, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_BOLD);
    m_titleLabel->SetFont(titleFont);
    m_titleLabel->SetForegroundColour(wxColour(226, 232, 240));
    titleBar->Add(m_titleLabel, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 8);

    titleBar->AddStretchSpacer();

    m_statusLabel = new wxStaticText(this, wxID_ANY, "", wxDefaultPosition,
        wxDefaultSize, wxALIGN_RIGHT | wxST_NO_AUTORESIZE);
    m_statusLabel->SetForegroundColour(wxColour(148, 163, 184));
    titleBar->Add(m_statusLabel, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
    m_mainSizer->Add(titleBar, 0, wxEXPAND | wxTOP | wxBOTTOM, 6);

    m_mainSizer->Add(new wxStaticLine(this), 0, wxEXPAND | wxLEFT | wxRIGHT, 4);

    // --- 主工作区: 端口列表 + 封装视图 ---
    wxBoxSizer* workArea = new wxBoxSizer(wxHORIZONTAL);

    // 左侧: 端口列表
    wxBoxSizer* portCol = new wxBoxSizer(wxVERTICAL);
    wxStaticText* portLabel = new wxStaticText(this, wxID_ANY, " Netlist Ports");
    portLabel->SetForegroundColour(wxColour(203, 213, 225));
    wxFont sectFont(9, wxFONTFAMILY_SWISS, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_BOLD);
    portLabel->SetFont(sectFont);
    portCol->Add(portLabel, 0, wxBOTTOM, 4);

    m_portList = new PortListCtrl(this);
    m_portList->Bind(wxEVT_LIST_ITEM_SELECTED,
        [this](wxListEvent& evt) { OnPortSelected(evt); });
    portCol->Add(m_portList, 1, wxEXPAND);
    workArea->Add(portCol, 0, wxEXPAND | wxLEFT | wxRIGHT, 4);

    // 右侧: 封装视图
    wxBoxSizer* pkgCol = new wxBoxSizer(wxVERTICAL);
    wxStaticText* pkgLabel = new wxStaticText(this, wxID_ANY, " QFN88 Package (Tang Nano 9K)");
    pkgLabel->SetForegroundColour(wxColour(203, 213, 225));
    pkgLabel->SetFont(sectFont);
    pkgCol->Add(pkgLabel, 0, wxBOTTOM, 4);

    m_packageView = new PackageView(this, m_pinDb);
    m_packageView->onPinClicked = [this](int pin) { OnPinClicked(pin); };
    pkgCol->Add(m_packageView, 1, wxEXPAND);

    // Bank 图例
    wxBoxSizer* legend = new wxBoxSizer(wxHORIZONTAL);
    auto addLegend = [&](wxColour c, const wxString& text) {
        wxWindow* dot = new wxWindow(this, wxID_ANY, wxDefaultPosition, wxSize(10, 10));
        dot->SetBackgroundColour(c);
        legend->Add(dot, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 2);
        wxStaticText* lt = new wxStaticText(this, wxID_ANY, text);
        lt->SetForegroundColour(wxColour(148, 163, 184));
        legend->Add(lt, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 10);
    };
    addLegend(wxColour(59, 130, 246), "Bank 0");
    addLegend(wxColour(168, 85, 247), "Bank 1");
    addLegend(wxColour(34, 197, 94), "Bound");
    addLegend(wxColour(60, 60, 65), "Reserved");
    pkgCol->Add(legend, 0, wxTOP, 4);
    workArea->Add(pkgCol, 1, wxEXPAND | wxLEFT | wxRIGHT, 4);

    m_mainSizer->Add(workArea, 1, wxEXPAND | wxALL, 6);

    // --- 板级资源快捷选择 ---
    wxBoxSizer* boardBar = new wxBoxSizer(wxHORIZONTAL);
    wxStaticText* brdLabel = new wxStaticText(this, wxID_ANY, "Board Resources: ");
    brdLabel->SetForegroundColour(wxColour(148, 163, 184));
    boardBar->Add(brdLabel, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);

    auto mkSmallBtn = [&](const wxString& label, wxButton*& btn) -> wxButton* {
        btn = new wxButton(this, wxID_ANY, label, wxDefaultPosition, wxSize(50, 22));
        btn->SetBackgroundColour(wxColour(51, 65, 85));
        btn->SetForegroundColour(wxColour(226, 232, 240));
        btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent& e) { OnBoardQuickSelect(e); });
        boardBar->Add(btn, 0, wxRIGHT, 4);
        return btn;
    };
    mkSmallBtn("LEDs", m_ledBtn);
    mkSmallBtn("BTNs", m_btnBtn);
    mkSmallBtn("UART", m_uartBtn);
    mkSmallBtn("CLK", m_clkBtn);
    mkSmallBtn("SPI", m_spiBtn);
    m_mainSizer->Add(boardBar, 0, wxLEFT | wxRIGHT | wxBOTTOM, 6);

    // --- 引脚表 ---
    wxStaticText* pinTblLabel = new wxStaticText(this, wxID_ANY, " Pin Table");
    pinTblLabel->SetForegroundColour(wxColour(203, 213, 225));
    pinTblLabel->SetFont(sectFont);
    m_mainSizer->Add(pinTblLabel, 0, wxLEFT | wxRIGHT | wxBOTTOM, 4);

    m_pinTable = new wxListCtrl(this, wxID_ANY, wxDefaultPosition, wxSize(-1, 150),
        wxLC_REPORT | wxLC_SINGLE_SEL | wxBORDER_SIMPLE);
    m_pinTable->AppendColumn("Pin#", wxLIST_FORMAT_LEFT, 45);
    m_pinTable->AppendColumn("Bank", wxLIST_FORMAT_LEFT, 40);
    m_pinTable->AppendColumn("IOLoc", wxLIST_FORMAT_LEFT, 70);
    m_pinTable->AppendColumn("Status", wxLIST_FORMAT_LEFT, 80);
    m_pinTable->AppendColumn("Resource", wxLIST_FORMAT_LEFT, 100);
    m_pinTable->Bind(wxEVT_LIST_ITEM_SELECTED,
        [this](wxListEvent& evt) { OnPinTableSelected(evt); });
    m_mainSizer->Add(m_pinTable, 0, wxEXPAND | wxLEFT | wxRIGHT, 6);

    // --- 属性编辑器 ---
    wxBoxSizer* propBar = new wxBoxSizer(wxHORIZONTAL);
    auto mkLabel = [&](const wxString& text) -> wxStaticText* {
        auto* l = new wxStaticText(this, wxID_ANY, text);
        l->SetForegroundColour(wxColour(148, 163, 184));
        propBar->Add(l, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 2);
        return l;
    };
    auto mkSpacer = [&]() { propBar->AddSpacer(10); };

    // Pin number
    mkLabel("Pin:");
    m_pinNumberText = new wxTextCtrl(this, wxID_ANY, "", wxDefaultPosition, wxSize(40, 22));
    propBar->Add(m_pinNumberText, 0, wxRIGHT, 6);

    mkSpacer();

    // IO_TYPE
    mkLabel("IO_TYPE:");
    m_ioTypeCombo = new wxComboBox(this, wxID_ANY, "LVCMOS33",
        wxDefaultPosition, wxSize(90, 22));
    for (const auto& t : GetKnownIOTypes()) m_ioTypeCombo->Append(t);
    m_ioTypeCombo->Bind(wxEVT_COMBOBOX,
        [this](wxCommandEvent& e) { OnPropertyChanged(e); e.Skip(); });
    propBar->Add(m_ioTypeCombo, 0, wxRIGHT, 6);

    mkSpacer();

    // Drive
    mkLabel("Drive:");
    m_driveCombo = new wxComboBox(this, wxID_ANY, "", wxDefaultPosition, wxSize(50, 22));
    m_driveCombo->Append(""); m_driveCombo->Append("4"); m_driveCombo->Append("8");
    m_driveCombo->Append("12"); m_driveCombo->Append("16"); m_driveCombo->Append("24");
    m_driveCombo->Bind(wxEVT_COMBOBOX,
        [this](wxCommandEvent& e) { OnPropertyChanged(e); e.Skip(); });
    propBar->Add(m_driveCombo, 0, wxRIGHT, 6);

    mkSpacer();

    // Pull
    m_pullUpCheck = new wxCheckBox(this, wxID_ANY, "Pull Up");
    m_pullUpCheck->Bind(wxEVT_CHECKBOX,
        [this](wxCommandEvent& e) { OnPropertyChanged(e); e.Skip(); });
    propBar->Add(m_pullUpCheck, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);

    m_pullDownCheck = new wxCheckBox(this, wxID_ANY, "Pull Down");
    m_pullDownCheck->Bind(wxEVT_CHECKBOX,
        [this](wxCommandEvent& e) { OnPropertyChanged(e); e.Skip(); });
    propBar->Add(m_pullDownCheck, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);

    mkSpacer();

    // Comment
    mkLabel("Comment:");
    m_commentText = new wxTextCtrl(this, wxID_ANY, "", wxDefaultPosition, wxSize(120, 22));
    m_commentText->Bind(wxEVT_TEXT,
        [this](wxCommandEvent& e) { OnPropertyChanged(e); e.Skip(); });
    propBar->Add(m_commentText, 0, wxRIGHT, 6);

    // Bind / Clear buttons
    m_bindBtn = new wxButton(this, wxID_ANY, "Bind", wxDefaultPosition, wxSize(50, 22));
    m_bindBtn->SetBackgroundColour(wxColour(34, 197, 94));
    m_bindBtn->SetForegroundColour(*wxBLACK);
    m_bindBtn->Bind(wxEVT_BUTTON,
        [this](wxCommandEvent& e) { OnBindPin(e); });
    propBar->Add(m_bindBtn, 0, wxRIGHT, 4);

    m_clearBtn = new wxButton(this, wxID_ANY, "Clear", wxDefaultPosition, wxSize(50, 22));
    m_clearBtn->SetBackgroundColour(wxColour(239, 68, 68));
    m_clearBtn->SetForegroundColour(*wxWHITE);
    m_clearBtn->Bind(wxEVT_BUTTON,
        [this](wxCommandEvent& e) { OnClearBinding(e); });
    propBar->Add(m_clearBtn, 0, wxRIGHT, 10);

    m_mainSizer->Add(propBar, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 6);

    // --- 底部操作栏 ---
    wxBoxSizer* actionBar = new wxBoxSizer(wxHORIZONTAL);
    auto mkActionBtn = [&](const wxString& label, wxColour bg, wxColour fg,
        wxButton*& btn) -> wxButton* {
        btn = new wxButton(this, wxID_ANY, label, wxDefaultPosition, wxSize(80, 26));
        btn->SetBackgroundColour(bg);
        btn->SetForegroundColour(fg);
        actionBar->Add(btn, 0, wxRIGHT, 6);
        return btn;
    };
    mkActionBtn("Validate", wxColour(59, 130, 246), *wxWHITE, m_validateBtn);
    m_validateBtn->Bind(wxEVT_BUTTON, [this](wxCommandEvent& e) { OnValidate(e); });
    mkActionBtn("Generate CST", wxColour(34, 197, 94), *wxBLACK, m_generateCstBtn);
    m_generateCstBtn->Bind(wxEVT_BUTTON, [this](wxCommandEvent& e) { OnGenerateCst(e); });
    mkActionBtn("Import CST", wxColour(234, 179, 8), *wxBLACK, m_importCstBtn);
    m_importCstBtn->Bind(wxEVT_BUTTON, [this](wxCommandEvent& e) { OnImportCst(e); });
    actionBar->AddStretchSpacer();
    mkActionBtn("Save", wxColour(51, 65, 85), wxColour(226, 232, 240), m_saveBtn);
    m_saveBtn->Bind(wxEVT_BUTTON, [this](wxCommandEvent& e) { OnSave(e); });
    m_mainSizer->Add(actionBar, 0, wxEXPAND | wxALL, 6);

    SetSizer(m_mainSizer);
}

void FpgaPinBindingPanel::LoadProject(const wxString& projectPath, const wxString& topModule) {
    m_projectPath = projectPath;
    m_topModule = topModule;
    m_ports.clear();

    // 初始化 sheet
    m_sheet = ConstraintSheet();
    m_sheet.targetProfileId = "tang-nano-9k";
    m_sheet.targetProfileVersion = "1.0.0";
    m_sheet.topModule = topModule;
    m_sheet.device = m_pinDb.device;

    // 尝试加载已存在的 pin-bindings.json
    wxString constraintsPath = GetConstraintsPath();
    if (wxFileExists(constraintsPath)) {
        wxString error;
        if (!LoadConstraintSheet(constraintsPath, m_sheet, error)) {
            wxLogWarning("Failed to load existing pin-bindings.json: %s", error);
        }
    }

    // 从 Yosys JSON 提取端口
    wxString jsonPath = FindYosysJsonPath(projectPath, topModule);
    if (!jsonPath.IsEmpty()) {
        ConstraintValidator validator(m_pinDb);
        wxString portErr;
        m_ports = validator.ExtractPortsFromYosysJson(jsonPath, topModule, portErr);
        if (m_ports.empty() && !portErr.IsEmpty()) {
            wxLogWarning("Port extraction warning: %s", portErr);
        }
    }

    // 确保每个端口在 sheet 中都有条目
    if (!m_ports.empty()) {
        m_sheet.bindings.erase(
            std::remove_if(m_sheet.bindings.begin(), m_sheet.bindings.end(),
                [this](const PinBinding& binding) {
                    const auto portIt = m_ports.find(binding.portName);
                    return portIt == m_ports.end() || binding.bitIndex < 0 ||
                        binding.bitIndex >= portIt->second.second;
                }),
            m_sheet.bindings.end());
    }

    std::set<wxString, WxStringLess> existingBindingKeys;
    for (const auto& b : m_sheet.bindings) {
        existingBindingKeys.insert(b.portName + "_" + wxString::Format("%d", b.bitIndex));
    }
    for (const auto& [name, info] : m_ports) {
        const auto& [dir, width] = info;
        for (int bit = 0; bit < width; ++bit) {
            wxString key = name + "_" + wxString::Format("%d", bit);
            if (!existingBindingKeys.count(key)) {
                PinBinding b;
                b.portName = name;
                b.bitIndex = bit;
                b.direction = dir;
                b.packagePin = -1;
                b.source = "gui";
                m_sheet.bindings.push_back(b);
            }
        }
    }

    Fresh();
}

void FpgaPinBindingPanel::Fresh() {
    Freeze();
    RefreshBindingsList();
    RefreshPinTable();
    RefreshStatus();
    Layout();
    FitInside();
    Thaw();
}

void FpgaPinBindingPanel::ClearForm() {
    m_ports.clear();
    m_sheet.bindings.clear();
    m_projectPath.Clear();
    m_topModule.Clear();
    m_portList->DeleteAllItems();
    m_pinTable->DeleteAllItems();
    m_statusLabel->SetLabel("");
}

void FpgaPinBindingPanel::RefreshBindingsList() {
    m_portList->PopulatePorts(m_ports, m_sheet);

    // 更新封装视图的已绑定高亮
    std::set<int> boundPins;
    for (const auto& b : m_sheet.bindings) {
        if (b.packagePin > 0) boundPins.insert(b.packagePin);
    }
    m_packageView->SetHighlightedPins(boundPins);
}

void FpgaPinBindingPanel::RefreshPinTable(const wxString& filter) {
    m_pinTable->Freeze();
    m_pinTable->DeleteAllItems();

    for (const auto& pin : m_pinDb.pins) {
        // 过滤
        if (!filter.IsEmpty()) {
            const auto* res = m_pinDb.FindResourceByPin(pin.pinNumber);
            if (!res || res->resourceType != filter) continue;
        }

        long idx = m_pinTable->InsertItem(m_pinTable->GetItemCount(),
            wxString::Format("%d", pin.pinNumber));
        m_pinTable->SetItem(idx, 1, wxString::Format("%d", pin.bank));
        m_pinTable->SetItem(idx, 2, pin.ioLocation);

        if (!pin.available) {
            m_pinTable->SetItem(idx, 3, pin.reservedReason);
            m_pinTable->SetItemTextColour(idx, wxColour(100, 100, 105));
        } else {
            // 检查是否已绑定
            bool bound = false;
            wxString resName;
            for (const auto& b : m_sheet.bindings) {
                if (b.packagePin == pin.pinNumber) { bound = true; resName = b.portName; break; }
            }
            if (bound) {
                m_pinTable->SetItem(idx, 3, wxString("Bound: ") + resName);
                m_pinTable->SetItemTextColour(idx, wxColour(34, 197, 94));
            } else {
                m_pinTable->SetItem(idx, 3, "Available");
                m_pinTable->SetItemTextColour(idx, wxColour(226, 232, 240));
            }
        }

        const auto* res = m_pinDb.FindResourceByPin(pin.pinNumber);
        if (res) {
            m_pinTable->SetItem(idx, 4, res->alias + ": " + res->description);
        }
    }

    m_pinTable->Thaw();
}

void FpgaPinBindingPanel::RefreshStatus() {
    int bound = 0, total = static_cast<int>(m_sheet.bindings.size());
    int unbound = 0, conflicts = 0;
    for (const auto& b : m_sheet.bindings) {
        if (b.packagePin > 0) bound++;
        else unbound++;
    }
    // 简单冲突检测: 统计重复引脚
    std::set<int> usedPins;
    for (const auto& b : m_sheet.bindings) {
        if (b.packagePin > 0) {
            if (usedPins.count(b.packagePin)) conflicts++;
            usedPins.insert(b.packagePin);
        }
    }

    wxColour statusColor = conflicts > 0 ? wxColour(239, 68, 68) :
        (unbound > 0 ? wxColour(234, 179, 8) : wxColour(34, 197, 94));
    wxString conflictText;
    if (conflicts > 0) {
        conflictText = wxString::Format(" | %d conflicts!", conflicts);
    }
    m_statusLabel->SetForegroundColour(statusColor);
    m_statusLabel->SetLabel(wxString::Format("%d bound / %d total | %d unbound%s",
        bound, total, unbound, conflictText));
}

void FpgaPinBindingPanel::UpdateBindingView() {
    RefreshBindingsList();
    RefreshPinTable();
    RefreshStatus();
}

// ==================== 事件处理 ====================

void FpgaPinBindingPanel::OnPortSelected(wxListEvent& evt) {
    long idx = evt.GetIndex();
    if (idx < 0) return;
    wxString portName = m_portList->GetItemText(idx, 0);

    const auto* binding = m_sheet.FindBinding(portName, 0);
    if (binding && binding->packagePin > 0) {
        m_packageView->SetSelectedPin(binding->packagePin);

        // 在引脚表中找到对应行
        long pinIdx = -1;
        for (long i = 0; i < m_pinTable->GetItemCount(); ++i) {
            long pinVal;
            if (m_pinTable->GetItemText(i).ToLong(&pinVal) &&
                pinVal == binding->packagePin) {
                pinIdx = i; break;
            }
        }
        if (pinIdx >= 0) {
            m_pinTable->EnsureVisible(pinIdx);
            m_pinTable->SetItemState(pinIdx, wxLIST_STATE_SELECTED, wxLIST_STATE_SELECTED);
        }

        m_pinNumberText->SetValue(wxString::Format("%d", binding->packagePin));
        m_ioTypeCombo->SetValue(binding->ioType);
        m_driveCombo->SetValue(binding->drive);
        m_pullUpCheck->SetValue(binding->pullUp);
        m_pullDownCheck->SetValue(binding->pullDown);
        m_commentText->SetValue(binding->comment);
    }
    evt.Skip();
}

void FpgaPinBindingPanel::OnPinTableSelected(wxListEvent& evt) {
    long idx = evt.GetIndex();
    if (idx < 0) return;
    long pinNum = 0;
    if (m_pinTable->GetItemText(idx).ToLong(&pinNum)) {
        m_packageView->SetSelectedPin(static_cast<int>(pinNum));
    }
    evt.Skip();
}

void FpgaPinBindingPanel::OnPinClicked(int pinNumber) {
    m_pinNumberText->SetValue(wxString::Format("%d", pinNumber));

    // 尝试自动绑定: 找到端口列表中当前选中的端口
    long selIdx = m_portList->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
    if (selIdx >= 0) {
        wxString portName = m_portList->GetItemText(selIdx, 0);
        auto* binding = m_sheet.FindBinding(portName, 0);
        if (binding) {
            binding->packagePin = pinNumber;
            binding->source = "gui";
            binding->ioType = m_ioTypeCombo->GetValue();
            binding->drive = m_driveCombo->GetValue();
            binding->pullUp = m_pullUpCheck->GetValue();
            binding->pullDown = m_pullDownCheck->GetValue();
            binding->comment = m_commentText->GetValue();
            UpdateBindingView();
        }
    }
}

void FpgaPinBindingPanel::OnBindPin(wxCommandEvent& evt) {
    long selIdx = m_portList->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
    if (selIdx < 0) {
        wxMessageBox("Please select a port from the port list first.", "Pin Binding",
            wxOK | wxICON_INFORMATION, this);
        return;
    }

    long pinNum = 0;
    if (!m_pinNumberText->GetValue().ToLong(&pinNum) || pinNum < 1 || pinNum > 88) {
        wxMessageBox("Please enter a valid pin number (1-88).", "Pin Binding",
            wxOK | wxICON_WARNING, this);
        return;
    }

    wxString portName = m_portList->GetItemText(selIdx, 0);
    auto* binding = m_sheet.FindBinding(portName, 0);
    if (!binding) {
        wxMessageBox("Internal error: port binding not found.", "Pin Binding",
            wxOK | wxICON_ERROR, this);
        return;
    }

    // 快速校验
    PinBinding candidate = *binding;
    candidate.packagePin = static_cast<int>(pinNum);
    auto errors = m_validator.ValidateBinding(candidate);
    bool hasBlockingError = false;
    for (const auto& e : errors) {
        if (e.IsError()) { hasBlockingError = true; break; }
    }
    if (hasBlockingError) {
        wxString msg = "Validation errors:\n";
        for (const auto& e : errors) {
            if (e.IsError()) msg += wxString("  - ") + e.message + "\n";
        }
        wxMessageBox(msg, "Pin Binding Validation", wxOK | wxICON_WARNING, this);
        return;
    }

    binding->packagePin = static_cast<int>(pinNum);
    binding->ioType = m_ioTypeCombo->GetValue();
    binding->drive = m_driveCombo->GetValue();
    binding->pullUp = m_pullUpCheck->GetValue();
    binding->pullDown = m_pullDownCheck->GetValue();
    binding->comment = m_commentText->GetValue();
    binding->source = "gui";

    UpdateBindingView();
}

void FpgaPinBindingPanel::OnClearBinding(wxCommandEvent& evt) {
    long selIdx = m_portList->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
    if (selIdx < 0) return;

    wxString portName = m_portList->GetItemText(selIdx, 0);
    auto* binding = m_sheet.FindBinding(portName, 0);
    if (binding) {
        binding->packagePin = -1;
        UpdateBindingView();
    }
}

void FpgaPinBindingPanel::OnPropertyChanged(wxCommandEvent& evt) {
    long selIdx = m_portList->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
    if (selIdx < 0) return;

    wxString portName = m_portList->GetItemText(selIdx, 0);
    auto* binding = m_sheet.FindBinding(portName, 0);
    if (!binding) return;

    // 按控件更新对应属性
    if (evt.GetEventObject() == m_ioTypeCombo) {
        binding->ioType = m_ioTypeCombo->GetValue();
    } else if (evt.GetEventObject() == m_driveCombo) {
        binding->drive = m_driveCombo->GetValue();
    } else if (evt.GetEventObject() == m_pullUpCheck) {
        binding->pullUp = m_pullUpCheck->GetValue();
        if (binding->pullUp) binding->pullDown = false;
        m_pullDownCheck->SetValue(false);
    } else if (evt.GetEventObject() == m_pullDownCheck) {
        binding->pullDown = m_pullDownCheck->GetValue();
        if (binding->pullDown) binding->pullUp = false;
        m_pullUpCheck->SetValue(false);
    } else if (evt.GetEventObject() == m_commentText) {
        binding->comment = m_commentText->GetValue();
    }
}

void FpgaPinBindingPanel::OnGenerateCst(wxCommandEvent& evt) {
    if (m_topModule.IsEmpty()) {
        wxMessageBox("No project loaded. Please open a project first.", "CST Generation",
            wxOK | wxICON_WARNING, this);
        return;
    }

    // 校验
    auto errors = m_validator.Validate(m_sheet, m_ports);
    bool hasError = false;
    for (const auto& e : errors) if (e.IsError()) { hasError = true; break; }

    if (hasError) {
        wxString msg = "Cannot generate CST with validation errors:\n";
        for (const auto& e : errors) {
            if (e.IsError()) msg += wxString("  - ") + e.message + "\n";
        }
        wxMessageBox(msg, "CST Generation", wxOK | wxICON_ERROR, this);
        return;
    }

    m_sheet.generatedAt = wxDateTime::Now().FormatISOCombined();

    CstGenerator gen;
    auto result = gen.Generate(m_sheet);

    if (!result.success) {
        wxMessageBox("CST generation failed.", "CST Generation", wxOK | wxICON_ERROR, this);
        return;
    }

    // 保存 CST
    wxString cstPath = GetCstPath();
    wxFileName cstFileName(cstPath);
    if (!cstFileName.DirExists() &&
        !cstFileName.Mkdir(wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL)) {
        wxMessageBox(wxString("Unable to create CST directory: ") + cstFileName.GetPath(),
                     "CST Generation", wxOK | wxICON_ERROR, this);
        return;
    }
    wxFile cstFile(cstPath, wxFile::write);
    if (!cstFile.IsOpened()) {
        wxMessageBox(wxString("Unable to write CST file: ") + cstPath, "CST Generation",
            wxOK | wxICON_ERROR, this);
        return;
    }
    const auto& content = result.cstContent;
    const wxScopedCharBuffer utf8 = content.ToUTF8();
    cstFile.Write(utf8.data(), utf8.length());
    cstFile.Close();

    // 保存 bindings
    wxString saveErr;
    if (!SaveConstraintSheet(GetConstraintsPath(), m_sheet, saveErr)) {
        wxMessageBox(wxString("CST generated but failed to save bindings: ") + saveErr,
            "CST Generation", wxOK | wxICON_WARNING, this);
        return;
    }

    wxMessageBox(wxString::Format("CST generated successfully!\n\n"
        "CST: %s\nBindings hash: %s\n",
        cstPath, result.bindingsHash),
        "CST Generation", wxOK | wxICON_INFORMATION, this);
    RefreshStatus();
}

void FpgaPinBindingPanel::OnImportCst(wxCommandEvent& evt) {
    wxFileDialog dlg(this, "Import CST File", m_projectPath, "",
        "CST files (*.cst)|*.cst|All files (*.*)|*.*",
        wxFD_OPEN | wxFD_FILE_MUST_EXIST);

    if (dlg.ShowModal() != wxID_OK) return;

    wxString cstPath = dlg.GetPath();
    wxFile file(cstPath, wxFile::read);
    if (!file.IsOpened()) {
        wxMessageBox("Cannot open CST file.", "Import CST", wxOK | wxICON_ERROR, this);
        return;
    }
    wxString content;
    file.ReadAll(&content);
    file.Close();

    std::vector<wxString> unmanagedLines;
    wxString importErr;
    CstGenerator gen;
    auto imported = gen.ImportCst(content, unmanagedLines, importErr);

    if (!unmanagedLines.empty()) {
        wxString msg = wxString::Format("CST imported with %zu bindings.\n\n"
            "The following %zu lines could not be parsed:\n",
            imported.size(), unmanagedLines.size());
        int showCount = std::min(10, static_cast<int>(unmanagedLines.size()));
        for (int i = 0; i < showCount; ++i) {
            msg += wxString("  ") + unmanagedLines[i] + "\n";
        }
        if (unmanagedLines.size() > 10) {
            msg += wxString::Format("  ... and %zu more lines.\n",
                unmanagedLines.size() - 10);
        }
        wxMessageBox(msg, "CST Import — Unmanaged Lines",
            wxOK | wxICON_WARNING, this);
    }

    // 合并到现有绑定
    for (auto& imp : imported) {
        auto* existing = m_sheet.FindBinding(imp.portName, imp.bitIndex);
        if (existing) {
            *existing = imp;  // 完全覆盖
        } else {
            m_sheet.bindings.push_back(imp);
        }
    }

    UpdateBindingView();
}

void FpgaPinBindingPanel::OnValidate(wxCommandEvent& evt) {
    if (m_ports.empty()) {
        wxMessageBox("No Yosys netlist ports found. Run FPGA Synthesis first.",
            "Validation", wxOK | wxICON_INFORMATION, this);
        return;
    }

    auto errors = m_validator.Validate(m_sheet, m_ports);

    long errorCount = 0, warningCount = 0;
    for (const auto& e : errors) {
        if (e.IsError()) errorCount++;
        else warningCount++;
    }

    if (errorCount == 0 && warningCount == 0) {
        wxMessageBox("All constraints are valid!", "Validation",
            wxOK | wxICON_INFORMATION, this);
    } else {
        wxString msg = wxString::Format("Validation results: %ld errors, %ld warnings\n\n",
            errorCount, warningCount);
        for (const auto& e : errors) {
            msg += wxString::Format("  [%s] %s\n",
                e.IsError() ? "ERROR" : "WARN", e.message);
        }
        wxMessageBox(msg, "Validation Results",
            errorCount > 0 ? wxOK | wxICON_ERROR : wxOK | wxICON_WARNING, this);
    }
}

void FpgaPinBindingPanel::OnSave(wxCommandEvent& evt) {
    if (m_projectPath.IsEmpty()) return;

    m_sheet.generatedAt = wxDateTime::Now().FormatISOCombined();
    wxString err;
    if (SaveConstraintSheet(GetConstraintsPath(), m_sheet, err)) {
        UpdateBindingView();
    } else {
        wxMessageBox(wxString("Failed to save: ") + err, "Save", wxOK | wxICON_ERROR, this);
    }
}

void FpgaPinBindingPanel::OnBoardQuickSelect(wxCommandEvent& evt) {
    wxString type;
    if (evt.GetEventObject() == m_ledBtn) type = "LED";
    else if (evt.GetEventObject() == m_btnBtn) type = "BUTTON";
    else if (evt.GetEventObject() == m_uartBtn) type = "UART";
    else if (evt.GetEventObject() == m_clkBtn) type = "CLK";
    else if (evt.GetEventObject() == m_spiBtn) type = "SPI";

    if (type.IsEmpty()) return;

    m_pinTable->Freeze();
    m_pinTable->DeleteAllItems();

    auto resources = m_pinDb.FindResourcesByType(type);
    for (const auto* res : resources) {
        const auto* pin = m_pinDb.FindPin(res->packagePin);
        if (!pin) continue;

        long idx = m_pinTable->InsertItem(m_pinTable->GetItemCount(),
            wxString::Format("%d", res->packagePin));
        m_pinTable->SetItem(idx, 1, wxString::Format("%d", pin->bank));
        m_pinTable->SetItem(idx, 2, pin->ioLocation);
        m_pinTable->SetItem(idx, 3, "Available");
        m_pinTable->SetItem(idx, 4, res->alias + ": " + res->description);
        m_pinTable->SetItemTextColour(idx, wxColour(34, 197, 94));
    }
    m_pinTable->Thaw();
}

wxString FpgaPinBindingPanel::GetConstraintsPath() const {
    return m_projectPath + "\\.sigflow\\fpga\\constraints\\pin-bindings.json";
}

wxString FpgaPinBindingPanel::GetCstPath() const {
    return m_projectPath + "\\constraints\\" + m_topModule + ".cst";
}
