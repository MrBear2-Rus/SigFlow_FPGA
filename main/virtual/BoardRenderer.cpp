#include "BoardRenderer.h"
#include "ui/UiTheme.h"
#include "eda/api/logic_state.hpp"
#include <algorithm>
#include <wx/pen.h>
#include <wx/brush.h>
namespace sigflow::virtual_lab {
namespace {
struct Geometry {
    double scale,x,y;
    explicit Geometry(wxWindow& w,bool phase1=false){scale=std::max(0.1,std::min(double(w.GetClientSize().x)/w.FromDIP(phase1?1220:820),double(w.GetClientSize().y)/w.FromDIP(360)));
        x=(w.GetClientSize().x-w.FromDIP(phase1?1200:800)*scale)/2;y=(w.GetClientSize().y-w.FromDIP(330)*scale)/2;}
};
eda::Json Node(const eda::Json& f,const std::string& name){if(f.contains("board") && f.at("board").contains("nodes"))return f.at("board").at("nodes").value(name,eda::Json::object());return eda::Json::object();}
}
NodeVisual ClassifyNode(const eda::Json& s){
    if(s.value("contention",false) || (s.value("state_code",0)&0x20))return NodeVisual::Contention;
    if(s.value("state_code",0)==2)return NodeVisual::Floating;if(!s.value("valid",false))return NodeVisual::Unknown;
    return s.value("logic",std::string("x"))=="0"?NodeVisual::Low:NodeVisual::High;
}
void BoardRenderer::Paint(wxWindow& w,wxGraphicsContext& g,const eda::Json& frame,const std::map<int,std::string>& targets,bool integrated){
    const auto& p=UiTheme::Palette();bool phase1=frame.value("phase1",false);Geometry a(w,phase1);g.Translate(a.x,a.y);g.Scale(a.scale,a.scale);
    auto dip=[&](int x){return w.FromDIP(x);};
    auto text=[&](const wxString& s,int x,int y,const wxColour& colour){g.SetFont(UiTheme::UiFont(&w,UiTheme::TextSize::P12),colour);g.DrawText(s,dip(x),dip(y));};
    g.SetPen(wxPen(p.strokeStrong,dip(1)));g.SetBrush(wxBrush(p.bgPanel));g.DrawRoundedRectangle(0,0,dip(800),dip(330),dip(16));
    text("Tang Nano 9K / 6202",24,14,p.text1);text(wxString::FromUTF8("27 MHz · QFN88 · CST"),520,14,p.text2);
    g.SetBrush(wxBrush(p.surfaceOnPanel));g.SetPen(wxPen(p.stroke,dip(1)));g.DrawRoundedRectangle(dip(330),dip(146),dip(145),dip(76),dip(10));
    text("GW1NR-9",354,161,p.text1);text("FPGA",380,184,p.text2);
    constexpr int pads[]={10,11,13,14,15,16};
    for(int i=0;i<6;++i){int x=65+i*128;auto node=Node(frame,"led"+std::to_string(i)+".K");auto visual=ClassifyNode(node);
        bool lit=node.value("valid",false) && !node.value("contention",false) && node.value("logic",std::string("x"))=="0";
        bool closed=true;if(frame.contains("board") && frame.at("board").contains("wires"))closed=frame.at("board").at("wires").value("wire.led"+std::to_string(i),true);
        bool pending=false;if(frame.contains("pending_faults"))for(const auto& f:frame.at("pending_faults"))if(f.value("edge_id",std::string())=="wire.led"+std::to_string(i))pending=true;
        wxPen wire(pending?p.accent:p.wire,dip(1),closed&&!pending?wxPENSTYLE_SOLID:wxPENSTYLE_SHORT_DASH);g.SetPen(wire);
        if(closed)g.StrokeLine(dip(x),dip(82),dip(x),dip(110));else{g.StrokeLine(dip(x),dip(82),dip(x-5),dip(91));g.StrokeLine(dip(x+5),dip(101),dip(x),dip(110));}
        auto colour=visual==NodeVisual::Contention?p.danger:(lit?p.warn:p.strokeStrong);auto fill=lit?p.warn:p.surfaceOnPanel;
        unsigned duty=0;if(phase1 && frame.contains("board") && frame.at("board").contains("leds")){const auto& observation=frame.at("board").at("leds").at("led"+std::to_string(i));if(observation.contains("integration"))duty=observation.at("integration").value("duty_ppm",0u);
            if(integrated && node.value("valid",false) && !node.value("contention",false)){auto mix=[&](unsigned off,unsigned on){return uint8_t((uint64_t(off)*(1000000-duty)+uint64_t(on)*duty)/1000000);};fill=wxColour(mix(p.surfaceOnPanel.Red(),p.warn.Red()),mix(p.surfaceOnPanel.Green(),p.warn.Green()),mix(p.surfaceOnPanel.Blue(),p.warn.Blue()));colour=duty?p.warn:p.strokeStrong;}}
        g.SetPen(wxPen(colour,dip(2)));g.SetBrush(wxBrush(fill));g.DrawEllipse(dip(x-10),dip(52),dip(20),dip(20));
        if(phase1)text(wxString::Format("D %.1f%%",duty/10000.0),x-30,131,p.text2);
        text(wxString::Format("LED%d / P%d",i,pads[i]),x-37,31,p.text2);
        if(pending)text("pending",x-22,113,p.accent);
        else if(!closed)text("OPEN",x-20,113,p.danger);
        else if(visual==NodeVisual::Floating)text("FLT",x-15,113,p.text3);
        else if(visual==NodeVisual::Contention)text("CON",x-17,113,p.danger);
        else text(wxString::FromUTF8(eda::virtual_board::StateName(uint8_t(node.value("state_code",0)))),x-16,113,p.text3);
    }
    for(int i=0;i<2;++i){int x=70+i*155,pad=3+i;auto s=Node(frame,"key_p"+std::to_string(pad)+".contact");
        auto buttonVisual=ClassifyNode(s);bool pending=false,pressed=buttonVisual==NodeVisual::Low;auto target=targets.find(pad);
        if(target!=targets.end() && frame.contains("pending_inputs") && frame.at("pending_inputs").contains(target->second)){
            pending=true;pressed=frame.at("pending_inputs").at(target->second).at("state")==13;
        }
        g.SetPen(wxPen(pending?p.accent:(buttonVisual==NodeVisual::Contention?p.danger:p.strokeStrong),dip(2),pending?wxPENSTYLE_SHORT_DASH:wxPENSTYLE_SOLID));g.SetBrush(wxBrush(pressed?p.selOnPanel:p.surfaceOnPanel));
        g.DrawRoundedRectangle(dip(x-35),dip(234),dip(95),dip(44),dip(8));text(wxString::Format("KEY P%d",pad),x-14,245,p.text1);
        text(pending?wxString::FromUTF8("待提交"):(buttonVisual==NodeVisual::Contention?"CON":(buttonVisual==NodeVisual::Floating?"FLT":wxString::FromUTF8(pressed?"按下":"松开"))),x-12,284,pending?p.accent:p.text2);
    }
    g.SetPen(wxPen(p.stroke,dip(1)));g.SetBrush(wxBrush(p.surfaceOnPanel));g.DrawRoundedRectangle(dip(550),dip(235),dip(205),dip(48),dip(8));
    bool clockClosed=true;if(frame.contains("board") && frame.at("board").contains("wires"))clockClosed=frame.at("board").at("wires").value("wire.clock",true);
    text(wxString::FromUTF8(clockClosed?"CLK27M → P52":"CLK27M / OPEN"),566,247,clockClosed?p.brand:p.danger);text(wxString::FromUTF8("FLT ○    CON !    OPEN /"),510,299,p.text3);
    if(phase1 && frame.contains("board") && frame.at("board").contains("peripherals")){
        g.SetPen(wxPen(p.stroke,dip(1)));g.SetBrush(wxBrush(p.surfaceOnPanel));g.DrawRoundedRectangle(dip(820),0,dip(380),dip(330),dip(12));text(wxString::FromUTF8(integrated?"外接器件 / 积分余晖":"外接器件 / 电气瞬时"),838,12,p.text1);
        for(const auto& [id,component]:frame.at("board").at("peripherals").items())if(component.value("model",std::string())=="sigflow.seven-segment-mux@1"){
            text(wxString::FromUTF8(id+"  scan="+component.value("scan_period_ticks",std::string("0"))+" ps"),838,36,p.text2);
            for(int digit=0;digit<4;++digit){int dx=846+digit*83,dy=70;int shapes[8][4]={{8,0,38,7},{46,8,7,39},{46,54,7,39},{8,94,38,7},{0,54,7,39},{0,8,7,39},{8,47,38,7},{59,94,8,8}};bool invalid=false;
                for(int segment=0;segment<8;++segment){const auto& observation=component.at("digits").at(digit).at(segment);bool valid=observation.at("instant_valid");invalid|=!valid;unsigned brightness=integrated?observation.at("duty_ppm").get<unsigned>():observation.at("instant_active").get<bool>()?1000000u:0u;
                    auto mix=[&](unsigned off,unsigned on){return uint8_t((uint64_t(off)*(1000000-brightness)+uint64_t(on)*brightness)/1000000);};auto tone=!valid?p.danger:wxColour(mix(p.canvas.Red(),p.warn.Red()),mix(p.canvas.Green(),p.warn.Green()),mix(p.canvas.Blue(),p.warn.Blue()));
                    g.SetPen(wxPen(tone,dip(1)));g.SetBrush(wxBrush(tone));g.DrawRoundedRectangle(dip(dx+shapes[segment][0]),dip(dy+shapes[segment][1]),dip(shapes[segment][2]),dip(shapes[segment][3]),dip(2));}
                if(invalid)text("FLT/CON",dx,180,p.danger);
            }
        }
        int group=0;for(const auto& [id,component]:frame.at("board").at("peripherals").items())if(component.value("model",std::string())=="sigflow.dip-switch@1"){
            text(wxString::FromUTF8(id+" / DIP"),838,218+group*65,p.text2);unsigned width=component.at("width"),value=component.at("value");for(unsigned bit=0;bit<width && bit<8;++bit){int x=842+int(bit)*40,y=244+group*65;g.SetPen(wxPen(p.strokeStrong,dip(1)));g.SetBrush(wxBrush(value&(1u<<bit)?p.selOnPanel:p.canvas));g.DrawRoundedRectangle(dip(x),dip(y),dip(30),dip(38),dip(4));text(value&(1u<<bit)?"1":"0",x+9,y+9,p.text1);}++group;
        }
    }
}
int BoardRenderer::HitButton(wxWindow& w,const wxPoint& point,bool phase1){Geometry a(w,phase1);
    for(int i=0;i<2;++i){int x=70+i*155;wxRect rect(int(a.x+w.FromDIP(x-35)*a.scale),int(a.y+w.FromDIP(234)*a.scale),int(w.FromDIP(95)*a.scale),int(w.FromDIP(44)*a.scale));if(rect.Contains(point))return 3+i;}return -1;
}
std::optional<std::pair<std::string,unsigned>> BoardRenderer::HitDip(wxWindow& w,const wxPoint& point,const eda::Json& frame){
    if(!frame.value("phase1",false) || !frame.contains("board") || !frame.at("board").contains("peripherals"))return {};Geometry geometry(w,true);int group=0;
    for(const auto& [id,component]:frame.at("board").at("peripherals").items())if(component.value("model",std::string())=="sigflow.dip-switch@1"){
        unsigned width=component.at("width");for(unsigned bit=0;bit<width && bit<8;++bit){wxRect rect(int(geometry.x+w.FromDIP(842+int(bit)*40)*geometry.scale),int(geometry.y+w.FromDIP(244+group*65)*geometry.scale),int(w.FromDIP(30)*geometry.scale),int(w.FromDIP(38)*geometry.scale));if(rect.Contains(point))return std::make_pair(id,bit);}++group;
    }return {};
}
}
