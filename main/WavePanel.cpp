#include "WavePanel.h"

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
    SetSizer(mainSizer);

    m_playBtn->Bind(wxEVT_BUTTON, &WavePanel::OnTogglePlay, this);
    m_slider->Bind(wxEVT_SLIDER, &WavePanel::OnSlider, this);
    zoomIn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { m_wavePanel->ZoomIn(); });
    zoomOut->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { m_wavePanel->ZoomOut(); });
    zoomReset->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { m_wavePanel->ZoomReset(); });

    m_timer = new wxTimer(this);
    Bind(wxEVT_TIMER, &WavePanel::OnTimer, this);
}

void WavePanel::OpenVcd() {
    wxCommandEvent evt;
    OnOpenVcd(evt);
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

