#pragma once
#include "VirtualSessionController.h"
#include "VirtualBoardPanel.h"
#include "wave/TraceViewPanel.h"
#include "ui/UiTheme.h"
#include <wx/scrolwin.h>
class wxChoice;class wxTextCtrl;class wxStaticText;class wxButton;
namespace sigflow::virtual_lab {
class VirtualTeachingPanel:public wxScrolledWindow {
public:
    VirtualTeachingPanel(wxWindow*,std::shared_ptr<VirtualSessionController>,VirtualBoardPanel*,wave::TraceViewPanel*);
    ~VirtualTeachingPanel();
    void StartCourse(const std::string& id);
    void MeasurePair(const std::string& left,const std::string& right);
    void RequestHint(const std::string& level);
    void SubmitGrade(const eda::Json& cause,const eda::Json& repair);
    void ArmCapture(const eda::Json& config);
    void ReadCapture();
    void SetBaseline();
    eda::Json CompareCapture();
    void CaptureRtl(const std::filesystem::path& project);
    const eda::Json& LastCapture()const{return capture_;}
    const eda::Json& LastComparison()const{return comparison_;}
    const eda::Json& RtlResult()const{return rtl_result_;}
    std::string OutputText()const;
    // Client rect of the readout control mapped into scrolled-panel coordinates;
    // used by the offscreen capture acceptance to verify real text pixels.
    wxRect ReadoutRegion()const;
    // 教学 Agent 联动桥：组合当前脱敏证据快照并交给宿主安装的 AI 助教通道。
    // 判分仍由可信侧 teaching.grade.submit 裁决；助教只接触学生投影。
    eda::Json ComposeEvidence()const;
    void SendToAgent();
    void SetAgentBridge(std::function<eda::Json(const eda::Json&)> bridge){agent_bridge_=std::move(bridge);}
private:
    void Update();void ApplyTheme();
    std::shared_ptr<VirtualSessionController> controller_;VirtualBoardPanel* board_;wave::TraceViewPanel* waves_;
    wxChoice *course_,*hint_,*cause_type_,*capture_mode_;wxTextCtrl *left_,*right_,*cause_,*repair_,*output_;wxStaticText* status_;wxButton* rtl_button_;
    std::function<eda::Json(const eda::Json&)> agent_bridge_;
    size_t subscription_;UiTheme::Subscription theme_;VirtualProjectSessionBuilder rtl_builder_;
    eda::Json capture_,baseline_,comparison_,rtl_result_,evidence_=eda::Json::array();std::string signature_,run_;unsigned probe_serial_=0;
    uint64_t capture_after_=0;
};
}
