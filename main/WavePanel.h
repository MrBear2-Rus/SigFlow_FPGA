#pragma once
#include <wx/wx.h>
#include <wx/filedlg.h>
#include <wx/listctrl.h>
#include <wx/timer.h>
#include <cstring>
#include <string>
#include <unordered_set>
#include <algorithm>
#include <map>
#include <random>

class CanvasNoteBook;

extern "C" {
#include "vcd.h"
}

// 配置常量
#define SIGNAL_ROW_HEIGHT 60
#define LEFT_MARGIN 180
#define WAVE_PADDING 40

class WaveformPanel : public wxPanel
{
public:
    WaveformPanel(wxWindow* parent, CanvasNoteBook* ca);
    void SetVcdData(vcd_t* vcdData);
    void SetCurrentTimestamp(int ts);
    void ZoomIn();
    void ZoomOut();
    void ZoomReset();
    void ClearVcdData();
    void FilterSignalsSmart(const std::vector<std::string>& keys);

    //void SetSignalVisiblity(std::vector<std::string> sigs);
    //void SetSignalVisible();
private:
    char ParseVcdValue(const char* v);
    void AssignSignalColors();

    std::vector<char> GetSignalValuesAtTime(int timestamp);

    void OnPaint(wxPaintEvent& event);
public:
    vcd_t* m_vcdData;
    int m_currentTimestamp, m_displayTimeRange, m_maxTimestamp;
    std::vector<signal_t*> m_allSignals;
    std::map<std::string, wxColour> m_signalColors;
    //std::map<std::string, bool> isPaint;
    std::mt19937 m_rng;

    CanvasNoteBook* canvas;
};

class WavePanel : public wxPanel
{
public:
    wxString m_projectPath;
    WavePanel(wxWindow* parent, CanvasNoteBook* ca);
    void OpenVCDFile(wxString path);
    void OpenVCDFileWithFilter(wxString path, std::vector<std::string> sigs);
    void OpenVcd();

    void SetProjectPath(const wxString& path);
    void ClearWavePanel();
    void AutoLoadVcd();
private:
    void OnOpenVcd(wxCommandEvent&);
    void OnTogglePlay(wxCommandEvent&);
    void OnTimer(wxTimerEvent&);
    void OnSlider(wxCommandEvent&);

    WaveformPanel* m_wavePanel;
    wxSlider* m_slider;
    wxButton* m_playBtn;
    wxTimer* m_timer;
};
