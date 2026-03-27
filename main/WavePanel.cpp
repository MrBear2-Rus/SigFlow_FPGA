#include "WavePanel.h"
#include <wx/filename.h>
#include <wx/dir.h>
#include <wx/choicdlg.h>
#include <wx/tokenzr.h>
WaveformPanel::WaveformPanel(wxWindow* parent)
    : wxPanel(parent), m_vcdData(nullptr), m_currentTimestamp(0),
    m_displayTimeRange(1000), m_maxTimestamp(1000)
{
    SetBackgroundColour(wxColour(240, 240, 240));  // 浅色背景
    SetDoubleBuffered(true);
    m_rng.seed(std::random_device{}());
    Bind(wxEVT_PAINT, &WaveformPanel::OnPaint, this);

}

void WaveformPanel::SetVcdData(vcd_t* vcdData)
{
    m_vcdData = vcdData;
    if (m_vcdData)
    {
        m_maxTimestamp = vcd_get_max_timestamp(m_vcdData);
        if (m_maxTimestamp <= 0) m_maxTimestamp = 1000;
        m_displayTimeRange = m_maxTimestamp; // 初始显示全貌

        m_allSignals.clear();
        std::unordered_set<std::string> addedSignals;
        for (size_t i = 0; i < m_vcdData->signals_count; ++i)
        {
            signal_t* sig = &m_vcdData->signals[i];
            if (addedSignals.find(sig->full_name) == addedSignals.end())
            {
                addedSignals.insert(sig->full_name);
                m_allSignals.push_back(sig);
            }
        }
        std::sort(m_allSignals.begin(), m_allSignals.end(), [](signal_t* a, signal_t* b) {
            return std::string(a->full_name) < std::string(b->full_name);
            });
        AssignSignalColors();
    }
    Refresh();
}

void WaveformPanel::SetCurrentTimestamp(int ts) { m_currentTimestamp = ts; Refresh(); }

void WaveformPanel::ZoomIn() {
    m_displayTimeRange = std::max(10, (int)(m_displayTimeRange * 0.7));
    Refresh();
}

void WaveformPanel::ZoomOut() {
    m_displayTimeRange = std::min(m_maxTimestamp * 2, (int)(m_displayTimeRange * 1.5));
    Refresh();
}

void WaveformPanel::ZoomReset() { m_displayTimeRange = m_maxTimestamp; Refresh(); }

void WaveformPanel::ClearVcdData() {
    m_vcdData = nullptr;
    m_allSignals.clear();
    Refresh();
}

char WaveformPanel::ParseVcdValue(const char* v) {
    if (!v || !v[0]) return '0';
    char c = tolower(v[0]);
    return (c == '1' || c == '0' || c == 'x' || c == 'z') ? c : '0';
}

void WaveformPanel::AssignSignalColors() {
    m_signalColors.clear();
    std::uniform_int_distribution<int> dist(100, 255);
    for (auto sig : m_allSignals) {
        m_signalColors[sig->full_name] = wxColour(dist(m_rng), dist(m_rng), dist(m_rng));
    }
}

void WaveformPanel::OnPaint(wxPaintEvent& event)
{
    wxPaintDC dc(this);
    wxSize size = GetSize();
    if (!m_vcdData || m_allSignals.empty()) {
        dc.SetTextForeground(*wxBLACK);  // 浅色主题的文本颜色
        dc.DrawText("Please Start Simulation first", size.x / 2 - 100, size.y / 2);  // 修改提示文本
        return;
    }

    int viewW = size.x - LEFT_MARGIN - WAVE_PADDING;
    if (viewW < 100) viewW = 100;

    // 比例尺：1 ps 对应多少像素
    double scale = (double)viewW / m_displayTimeRange;
    int timeAxisY = 25;

    // 绘制时间轴和网格
    dc.SetPen(wxPen(wxColour(60, 60, 60), 1, wxPENSTYLE_DOT));
    dc.SetTextForeground(wxColour(150, 150, 150));
    int step = std::max(1, m_displayTimeRange / 10);
    for (int ts = 0; ts <= m_displayTimeRange; ts += step) {
        int x = LEFT_MARGIN + (int)(ts * scale);
        dc.DrawLine(x, timeAxisY, x, size.y);
        dc.DrawText(wxString::Format("%d", ts), x - 5, 5);
    }

    // 遍历信号绘制波形
    for (int i = 0; i < (int)m_allSignals.size(); ++i) {
        signal_t* sig = m_allSignals[i];
        int yBase = timeAxisY + i * SIGNAL_ROW_HEIGHT + 30;
        int yH = yBase - 15, yL = yBase + 15;

        // 绘制标签
        dc.SetTextForeground(*wxBLACK);  // 浅色主题文本颜色
        dc.DrawText(sig->full_name, 10, yBase - 8);

        // 获取当前时间戳的值（用于实时显示）
        char valAtCursor = '0';

        dc.SetPen(wxPen(m_signalColors[sig->full_name], 2));
        int lastX = LEFT_MARGIN;
        char lastVal = '0';

        for (size_t c = 0; c < sig->changes_count; ++c) {
            int ts = sig->value_changes[c].timestamp;
            if (ts > m_displayTimeRange) break;

            int currX = LEFT_MARGIN + (int)(ts * scale);
            char currVal = ParseVcdValue(sig->value_changes[c].value);

            // 画水平线
            int drawY = (lastVal == '0') ? yL : yH;
            dc.DrawLine(lastX, drawY, currX, drawY);
            // 画垂直跳变
            dc.SetPen(wxPen(wxColour(100, 100, 100), 1));
            dc.DrawLine(currX, yH, currX, yL);
            dc.SetPen(wxPen(m_signalColors[sig->full_name], 2));

            if (ts <= m_currentTimestamp) valAtCursor = currVal;

            lastX = currX;
            lastVal = currVal;
        }
        // 补全最后一段
        int endX = LEFT_MARGIN + (int)(m_displayTimeRange * scale);
        dc.DrawLine(lastX, (lastVal == '0' ? yL : yH), endX, (lastVal == '0' ? yL : yH));

        // 显示实时值
        dc.SetTextForeground(wxColour(0, 255, 0));
        dc.DrawText(wxString::Format("= %c", valAtCursor), 120, yBase - 8);
    }

    // 绘制红色播放头
    int cursorX = LEFT_MARGIN + (int)(m_currentTimestamp * scale);
    if (cursorX >= LEFT_MARGIN && cursorX <= LEFT_MARGIN + viewW) {
        dc.SetPen(wxPen(*wxRED, 2));
        dc.DrawLine(cursorX, timeAxisY, cursorX, size.y);
    }
}


#include "WavePanel.h"

WavePanel::WavePanel(wxWindow* parent) : wxPanel(parent, wxID_ANY)
{
    // UI 布局
    auto mainSizer = new wxBoxSizer(wxVERTICAL);
    m_wavePanel = new WaveformPanel(this);
    mainSizer->Add(m_wavePanel, 1, wxEXPAND | wxALL, 0);

    auto ctrlSizer = new wxBoxSizer(wxHORIZONTAL);

    m_playBtn = new wxButton(this, wxID_ANY, "Play");
    m_slider = new wxSlider(this, wxID_ANY, 0, 0, 1000, wxDefaultPosition, wxSize(300, -1));

    auto zoomIn = new wxButton(this, wxID_ANY, "Zoom +");
    auto zoomOut = new wxButton(this, wxID_ANY, "Zoom -");
    auto zoomReset = new wxButton(this, wxID_ANY, "Reset");

    ctrlSizer->Add(m_playBtn, 0, wxALL, 5);
    ctrlSizer->Add(m_slider, 1, wxEXPAND | wxALL, 5);
    ctrlSizer->Add(zoomIn, 0, wxALL, 5);
    ctrlSizer->Add(zoomOut, 0, wxALL, 5);
    ctrlSizer->Add(zoomReset, 0, wxALL, 5);

    mainSizer->Add(ctrlSizer, 0, wxEXPAND | wxALL, 5);
    auto autoBtn = new wxButton(this, wxID_ANY, "Auto Load");
    ctrlSizer->Add(autoBtn, 0, wxALL, 5);

    auto selectBtn = new wxButton(this, wxID_ANY, "Select Signals");
    ctrlSizer->Add(selectBtn, 0, wxALL, 5);

    selectBtn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {

        wxTextEntryDialog dlg(
            this,
            "Input alias OR signal name (comma separated)\n"
            "Example:\n"
            "  #,$        (alias)\n"
            "  sum,cout   (name)",
            "Select Signals"
        );

        if (dlg.ShowModal() != wxID_OK)
            return;

        wxString text = dlg.GetValue();

        std::vector<std::string> keys;
        wxStringTokenizer tokenizer(text, ",");

        while (tokenizer.HasMoreTokens())
        {
            wxString token = tokenizer.GetNextToken();
            token.Trim(true);
            token.Trim(false);

            if (!token.IsEmpty())
                keys.push_back(token.ToStdString());
        }

        // ✅ 正确调用
        m_wavePanel->FilterSignalsSmart(keys);

        // ✅ 正确更新 slider
        m_slider->SetRange(0, m_wavePanel->m_maxTimestamp);
        m_slider->SetValue(0);
        });

    autoBtn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        AutoLoadVcd();
        });
    SetSizer(mainSizer);

    m_playBtn->Bind(wxEVT_BUTTON, &WavePanel::OnTogglePlay, this);
    m_slider->Bind(wxEVT_SLIDER, &WavePanel::OnSlider, this);
    zoomIn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { m_wavePanel->ZoomIn(); });
    zoomOut->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { m_wavePanel->ZoomOut(); });
    zoomReset->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { m_wavePanel->ZoomReset(); });

    m_timer = new wxTimer(this);
    Bind(wxEVT_TIMER, &WavePanel::OnTimer, this);
}

void WavePanel::OnOpenVcd(wxCommandEvent&) {
    wxFileDialog openDlg(this, "Open VCD", "", "", "VCD files (*.vcd)|*.vcd", wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    if (openDlg.ShowModal() == wxID_OK) {
        wxString pathStr = openDlg.GetPath();
        std::string stdPath = pathStr.ToStdString();
        // 强转成 char* 以匹配 vcd_read_from_path 的参数要求
        vcd_t* data = vcd_read_from_path(const_cast<char*>(stdPath.c_str()));
        if (data) {
            m_wavePanel->SetVcdData(data);
            m_slider->SetRange(0, m_wavePanel->m_maxTimestamp);
            m_slider->SetValue(0);
        }
    }
}

void WavePanel::OnTogglePlay(wxCommandEvent&) {
    if (m_timer->IsRunning()) {
        m_timer->Stop();
        m_playBtn->SetLabel("Play");
    }
    else {
        m_timer->Start(50);
        m_playBtn->SetLabel("Pause");
    }
}

void WavePanel::OnTimer(wxTimerEvent&) {
    int val = m_slider->GetValue();
    if (val < m_slider->GetMax()) {
        val += std::max(1, m_wavePanel->m_displayTimeRange / 100); // 步进随缩放调整
        m_slider->SetValue(val);
        m_wavePanel->SetCurrentTimestamp(val);
    }
    else {
        m_timer->Stop();
        m_playBtn->SetLabel("Play");
    }
}

void WavePanel::OnSlider(wxCommandEvent&) {
    m_wavePanel->SetCurrentTimestamp(m_slider->GetValue());
}

void WavePanel::OpenVCDFile(wxString path)
{
    vcd_t* data = vcd_read_from_path(const_cast<char*>(path.ToStdString().c_str()));
    if (data) {
        m_wavePanel->SetVcdData(data);
        m_slider->SetRange(0, m_wavePanel->m_maxTimestamp);
        m_slider->SetValue(0);
    }
}

void WavePanel::ClearWavePanel()
{
    m_wavePanel->ClearVcdData();
    m_slider->SetValue(0);
    m_slider->SetRange(0, 1000);
}

void WavePanel::SetProjectPath(const wxString& path)
{
    m_projectPath = path;
}

void WavePanel::AutoLoadVcd()
{
    if (m_projectPath.IsEmpty()) {
        wxMessageBox("No project loaded!");
        return;
    }

    // 扫描 <project>/.sigflow/sim/ 下所有 <topModule>/waveform/wave.vcd
    wxFileName simDir(m_projectPath, "");
    simDir.AppendDir(".sigflow");
    simDir.AppendDir("sim");
    wxString simDirPath = simDir.GetPath();

    if (!wxDir::Exists(simDirPath)) {
        wxMessageBox(wxT("尚未编译仿真，未找到 .sigflow/sim 目录"));
        return;
    }

    wxDir dir(simDirPath);
    if (!dir.IsOpened()) return;

    std::vector<std::pair<wxString, wxString>> found; // {topModule, fullPath}
    wxString subDirName;
    bool hasDir = dir.GetFirst(&subDirName, wxEmptyString, wxDIR_DIRS);
    while (hasDir) {
        wxFileName vcdPath(simDirPath, "");
        vcdPath.AppendDir(subDirName);
        vcdPath.AppendDir("waveform");
        vcdPath.SetFullName("wave.vcd");
        wxString full = vcdPath.GetFullPath();
        if (wxFileExists(full)) {
            found.push_back({ subDirName, full });
        }
        hasDir = dir.GetNext(&subDirName);
    }

    if (found.empty()) {
        wxMessageBox(wxT("未找到任何 VCD 波形文件\n请先编译并运行仿真 (F5 → F6)"));
        return;
    }

    wxString chosen;
    if (found.size() == 1) {
        chosen = found[0].second;
    } else {
        wxArrayString choices;
        for (auto& [mod, path] : found)
            choices.Add(mod);
        int sel = wxGetSingleChoiceIndex(
            wxT("检测到多个仿真结果，请选择要加载的顶层模块:"),
            wxT("选择波形"), choices, this);
        if (sel < 0) return;
        chosen = found[sel].second;
    }

    OpenVCDFile(chosen);
}

void WaveformPanel::FilterSignalsSmart(const std::vector<std::string>& keys)
{
    if (!m_vcdData) return;

    m_allSignals.clear();

    for (size_t i = 0; i < m_vcdData->signals_count; ++i)
    {
        signal_t* sig = &m_vcdData->signals[i];

        if (keys.empty())
        {
            m_allSignals.push_back(sig);
            continue;
        }

        std::string alias = sig->signal_id;   // VCD别名 (#,$...)
        std::string name = sig->full_name;   // 完整名 TOP.uut.xxx

        for (const auto& k : keys)
        {
            // 1️⃣ alias 精确匹配
            if (alias == k)
            {
                m_allSignals.push_back(sig);
                break;
            }

            // 2️⃣ 名字模糊匹配
            if (name.find(k) != std::string::npos)
            {
                m_allSignals.push_back(sig);
                break;
            }
        }
    }

    AssignSignalColors();
    Refresh();
}
