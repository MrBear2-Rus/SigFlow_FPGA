// W2 渲染层 smoke 测试：GL 与软件双后端、缩放/平移/边沿跳转。
#include "../../main/trace/TraceCache.h"
#include "../../main/trace/VcdLazyTraceSource.h"
#include "../../main/wave/WaveCompareHub.h"
#include "../../main/wave/WaveSession.h"
#include "../../main/wave/WaveformView.h"

#include <wx/app.h>
#include <wx/filename.h>
#include <wx/frame.h>
#include <wx/sizer.h>
#include <wx/timer.h>

#include <fstream>
#include <memory>
#include <sstream>
#include <string>

using namespace sigflow::trace;
using namespace sigflow::wave;

namespace {

std::string Bin(unsigned value, unsigned width)
{
    std::string result(width, '0');
    for (unsigned i = 0; i < width; ++i) {
        result[width - 1 - i] = (value & (1u << i)) ? '1' : '0';
    }
    return result;
}

std::string BuildVcd(TimeValue maxTime)
{
    std::ostringstream out;
    out << "$timescale 1ns $end\n";
    out << "$scope module TOP $end\n";
    out << "$var wire 1 ! clk $end\n";
    out << "$var reg 4 \" cnt $end\n";
    out << "$var wire 8 # data $end\n";
    out << "$upscope $end\n";
    out << "$enddefinitions\n";
    out << "$dumpvars\n0!\nb0000 \"\nb00000000 #\n$end\n";
    for (TimeValue t = 0; t <= maxTime; t += 5) {
        out << "#" << t << "\n";
        out << (((t / 5) % 2) ? "1!" : "0!") << "\n";
        if (t % 10 == 0) {
            out << "b" << Bin(static_cast<unsigned>((t / 10) % 16), 4) << " \"\n";
            out << "b" << Bin(static_cast<unsigned>((t / 10) % 256), 8) << " #\n";
        }
    }
    return out.str();
}

void WriteResult(bool ok, const std::string& detail)
{
    const std::string path =
        wxFileName::GetTempDir().ToStdString() + "\\waveview_smoke_result.txt";
    std::ofstream out(path, std::ios::trunc);
    out << (ok ? "PASS" : "FAIL") << "\n" << detail << "\n";
}

} // namespace

class SmokeApp : public wxApp {
public:
    bool OnInit() override;
    void OnTimer(wxTimerEvent& event);

private:
    wxFrame* m_frame = nullptr;
    WaveformView* m_glView = nullptr;
    WaveformView* m_swView = nullptr;
    std::shared_ptr<CachingTraceSource> m_source;
    wxTimer m_timer;
    int m_step = 0;
    bool m_sessionPassed = true;
    std::string m_sessionError;
};

wxIMPLEMENT_APP(SmokeApp);

bool SmokeApp::OnInit()
{
    const std::string path =
        wxFileName::GetTempDir().ToStdString() + "\\sigflow_waveview_smoke.vcd";
    {
        std::ofstream out(path, std::ios::trunc);
        out << BuildVcd(50000);
    }

    auto inner = std::make_unique<VcdLazyTraceSource>();
    m_source = std::make_shared<CachingTraceSource>(std::move(inner), 8 * 1024 * 1024);
    std::string error;
    if (!m_source->Open(path, error)) {
        WriteResult(false, "open failed: " + error);
        return false;
    }

    m_frame = new wxFrame(nullptr, wxID_ANY, "WaveView Smoke",
                          wxDefaultPosition, wxSize(1100, 640));
    wxBoxSizer* root = new wxBoxSizer(wxVERTICAL);
    wxString softwareEnv;
    const bool softwareOnly =
        wxGetEnv("WAVEVIEW_SOFTWARE_ONLY", &softwareEnv) && softwareEnv == "1";
    m_glView = new WaveformView(m_frame, softwareOnly);
    m_swView = new WaveformView(m_frame, true);
    root->Add(m_glView, 1, wxEXPAND);
    root->Add(m_swView, 1, wxEXPAND);
    m_frame->SetSizer(root);
    m_glView->SetTraceSource(m_source);
    m_swView->SetTraceSource(m_source);
    m_frame->Show();

    m_timer.SetOwner(this);
    Bind(wxEVT_TIMER, &SmokeApp::OnTimer, this);
    m_timer.Start(700);
    return true;
}

void SmokeApp::OnTimer(wxTimerEvent&)
{
    ++m_step;
    switch (m_step) {
    case 1:
        m_glView->ZoomIn();
        m_swView->ZoomIn();
        break;
    case 2:
        m_glView->PanTime(0.15);
        m_swView->PanTime(0.15);
        break;
    case 3:
        m_glView->JumpToEdge(true);
        m_swView->JumpToEdge(true);
        break;
    case 4:
        // W3：A-B 测量、Marker、播放头、事件
        {
            m_glView->SetAB(0, 100);
            m_swView->SetAB(0, 100);
            m_glView->AddMarkerAt(25);
            m_swView->AddMarkerAt(50);
            m_glView->SetPlayhead(30);
            std::vector<sigflow::wave::WaveEvent> events;
            events.push_back({ 20, "reset" });
            events.push_back({ 45, "trigger" });
            m_glView->SetEvents(events);
            m_swView->SetEvents(events);
        }
        break;
    case 5:
        // W3：Compare 联动（改一个视图时间窗，另一个应同步）
        m_glView->PanTime(0.05);
        break;
    case 6:
        // W3：会话保存/恢复往返
        {
            const sigflow::wave::WaveSessionData captured = m_glView->CaptureSession();
            const std::string sessionPath =
                wxFileName::GetTempDir().ToStdString() + "\\sigflow_w3_session.bws";
            std::string error;
            const bool saved = sigflow::wave::WaveSessionSave(captured, sessionPath, error);
            if (!saved) {
                m_sessionPassed = false;
                m_sessionError = "session save failed: " + error;
            }
            sigflow::wave::WaveSessionData loaded;
            const bool loadedOk =
                sigflow::wave::WaveSessionLoad(sessionPath, loaded, error);
            if (!loadedOk) {
                m_sessionPassed = false;
                m_sessionError = "session load failed: " + error;
            }
            m_swView->ApplySession(loaded);
            wxRemoveFile(sessionPath);
        }
        break;
    case 7:
        m_glView->ZoomReset();
        m_swView->ZoomReset();
        break;
    default: {
        const WaveViewState& glState = m_glView->State();
        const WaveViewState& swState = m_swView->State();
        const bool passed = glState.valid && swState.valid &&
                            glState.timeSpan >= 1 && swState.timeSpan >= 1 &&
                            glState.visibleSignalIds.size() == 3 &&
                            swState.visibleSignalIds.size() == 3 &&
                            m_sessionPassed &&
                            glState.hasAB && glState.markers.size() == 1 &&
                            glState.events.size() == 2 &&
                            glState.timeOffset == swState.timeOffset &&
                            glState.timeSpan == swState.timeSpan &&
                            swState.playhead == glState.playhead &&
                            !m_glView->MeasurementText().empty();
        std::ostringstream detail;
        detail << "gl_backend=" << (m_glView->UsingOpenGL() ? "on" : "off")
               << " sw_backend=" << (m_swView->UsingOpenGL() ? "on" : "off")
               << " signals=" << glState.visibleSignalIds.size()
               << " span_gl=" << glState.timeSpan
               << " span_sw=" << swState.timeSpan
               << " offset_gl=" << glState.timeOffset
               << " offset_sw=" << swState.timeOffset
               << " markers=" << glState.markers.size()
               << " events=" << glState.events.size()
               << " measure=" << (m_glView->MeasurementText().empty() ? "empty" : "ok")
               << " session=" << (m_sessionPassed ? "ok" : m_sessionError);
        WriteResult(passed, detail.str());
        m_frame->Destroy();
        ExitMainLoop();
        break;
    }
    }
}
