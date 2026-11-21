#include "BoardRenderer.h"
#include "ui/UiTheme.h"
#include "eda/api/logic_state.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <set>
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
BoardRenderer::SlotStatus BoardRenderer::LedSlotStatus(const eda::Json& frame,int index){
    constexpr int pads[]={10,11,13,14,15,16};
    if(index<0 || index>=6)return {};
    int pad=pads[index];
    bool hasNodes=frame.contains("board") && frame.at("board").contains("nodes") && frame.at("board").at("nodes").is_object() && !frame.at("board").at("nodes").empty();
    std::string ledNode="led"+std::to_string(index)+".K";
    if(!hasNodes){
        return {true,false,"LED"+std::to_string(index)+" / P"+std::to_string(pad),"UND"};
    }
    const auto& nodes=frame.at("board").at("nodes");
    if(nodes.contains(ledNode)){
        const auto& node=nodes.at(ledNode);
        auto visual=ClassifyNode(node);
        std::string st=visual==NodeVisual::Floating?"FLT":visual==NodeVisual::Contention?"CON":eda::virtual_board::StateName(uint8_t(node.value("state_code",0)));
        return {true,false,"LED"+std::to_string(index)+" / P"+std::to_string(pad),st};
    }
    std::string padNodeName="pad."+std::to_string(pad);
    if(nodes.contains(padNodeName)){
        const auto& padNode=nodes.at(padNodeName);
        auto visual=ClassifyNode(padNode);
        std::string st=visual==NodeVisual::Floating?"FLT":visual==NodeVisual::Contention?"CON":eda::virtual_board::StateName(uint8_t(padNode.value("state_code",0)));
        return {false,true,"P"+std::to_string(pad)+" → 外接",st};
    }
    return {false,false,"P"+std::to_string(pad)+" / 未挂载","未挂载"};
}
BoardRenderer::SlotStatus BoardRenderer::KeySlotStatus(const eda::Json& frame,int pad){
    bool hasNodes=frame.contains("board") && frame.at("board").contains("nodes") && frame.at("board").at("nodes").is_object() && !frame.at("board").at("nodes").empty();
    std::string keyNode="key_p"+std::to_string(pad)+".contact";
    if(!hasNodes || frame.at("board").at("nodes").contains(keyNode)){
        const auto node=hasNodes?frame.at("board").at("nodes").at(keyNode):eda::Json::object();
        auto visual=ClassifyNode(node);
        std::string st=visual==NodeVisual::Contention?"CON":(visual==NodeVisual::Floating?"FLT":(visual==NodeVisual::Low?"按下":"松开"));
        return {true,false,"KEY P"+std::to_string(pad),st};
    }
    return {false,false,"KEY P"+std::to_string(pad),"未挂载"};
}
void BoardRenderer::Paint(wxWindow& w,wxGraphicsContext& g,const eda::Json& frame,const std::map<int,std::string>& targets,bool integrated){
    const auto& p=UiTheme::Palette();bool phase1=frame.value("phase1",false);Geometry a(w,phase1);g.Translate(a.x,a.y);g.Scale(a.scale,a.scale);
    auto dip=[&](int x){return w.FromDIP(x);};
    auto text=[&](const wxString& s,int x,int y,const wxColour& colour){g.SetFont(UiTheme::UiFont(&w,UiTheme::TextSize::P12),colour);g.DrawText(s,dip(x),dip(y));};
    g.SetPen(wxPen(p.strokeStrong,dip(1)));g.SetBrush(wxBrush(p.bgPanel));g.DrawRoundedRectangle(0,0,dip(800),dip(330),dip(16));
    text("Tang Nano 9K / 6202",24,14,p.text1);text(wxString::FromUTF8(frame.contains("frequency_hz")?frame.at("frequency_hz").get<std::string>()+" Hz · QFN88 · CST":"27 MHz · QFN88 · CST"),520,14,p.text2);
    g.SetBrush(wxBrush(p.surfaceOnPanel));g.SetPen(wxPen(p.stroke,dip(1)));g.DrawRoundedRectangle(dip(330),dip(146),dip(145),dip(76),dip(10));
    text("GW1NR-9",354,161,p.text1);text("FPGA",380,184,p.text2);
    constexpr int pads[]={10,11,13,14,15,16};
    for(int i=0;i<6;++i){int x=65+i*128;auto slot=LedSlotStatus(frame,i);
        if(!slot.mounted){
            g.SetPen(wxPen(p.stroke,dip(1),wxPENSTYLE_SHORT_DASH));
            g.StrokeLine(dip(x),dip(82),dip(x),dip(110));
            g.SetPen(wxPen(p.stroke,dip(2)));g.SetBrush(wxBrush(p.surfaceOnPanel));g.DrawEllipse(dip(x-10),dip(52),dip(20),dip(20));
            if(phase1)text(wxString::FromUTF8(slot.multiplexed?"复用引脚":"未挂载"),x-30,131,p.text3);
            text(wxString::FromUTF8(slot.label),x-37,31,p.text3);
            bool isCon=slot.state_text=="CON";
            text(wxString::FromUTF8(slot.state_text),x-17,113,isCon?p.danger:p.text3);
            continue;
        }
        auto node=Node(frame,"led"+std::to_string(i)+".K");auto visual=ClassifyNode(node);
        bool lit=node.value("valid",false) && !node.value("contention",false) && node.value("logic",std::string("x"))=="0";
        bool closed=true;if(frame.contains("board") && frame.at("board").contains("wires"))closed=frame.at("board").at("wires").value("wire.led"+std::to_string(i),true);
        bool pending=false;if(frame.contains("pending_faults"))for(const auto& f:frame.at("pending_faults"))if(f.value("edge_id",std::string())=="wire.led"+std::to_string(i))pending=true;
        wxPen wire(pending?p.accent:p.wire,dip(1),closed&&!pending?wxPENSTYLE_SOLID:wxPENSTYLE_SHORT_DASH);g.SetPen(wire);
        if(closed)g.StrokeLine(dip(x),dip(82),dip(x),dip(110));else{g.StrokeLine(dip(x),dip(82),dip(x-5),dip(91));g.StrokeLine(dip(x+5),dip(101),dip(x),dip(110));}
        auto colour=visual==NodeVisual::Contention?p.danger:(lit?p.warn:p.strokeStrong);auto fill=lit?p.warn:p.surfaceOnPanel;
        unsigned duty=0;if(phase1 && frame.contains("board") && frame.at("board").contains("leds") && frame.at("board").at("leds").contains("led"+std::to_string(i))){const auto& observation=frame.at("board").at("leds").at("led"+std::to_string(i));if(observation.contains("integration"))duty=observation.at("integration").value("duty_ppm",0u);
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
    for(int i=0;i<2;++i){int x=70+i*155,pad=3+i;auto keySlot=KeySlotStatus(frame,pad);
        if(!keySlot.mounted){
            g.SetPen(wxPen(p.stroke,dip(1),wxPENSTYLE_SHORT_DASH));g.SetBrush(wxBrush(p.bgPanel));
            g.DrawRoundedRectangle(dip(x-35),dip(234),dip(95),dip(44),dip(8));
            text(wxString::FromUTF8(keySlot.label),x-14,245,p.text3);
            text(wxString::FromUTF8(keySlot.state_text),x-14,284,p.text3);
            continue;
        }
        auto s=Node(frame,"key_p"+std::to_string(pad)+".contact");
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
        // Phase 4 advanced peripherals: matrix keypad grid, SPI OLED dot matrix
        // and the I2C open-drain badge share the tray with the older devices.
        // Rendering stays on plain pen/brush primitives with prebuilt strings so
        // no formatting helper can raise an assertion inside a paint pass.
        bool advanced=false;
        auto label=[&](const std::string& s,int x,int y,const wxColour& colour){text(wxString::FromUTF8(s),x,y,colour);};
        auto hexByte=[](unsigned value){const char* digits="0123456789abcdef";std::string out="00";out[0]=digits[(value>>4)&0xF];out[1]=digits[value&0xF];return out;};
        auto drawKeypad=[&](const eda::Json& component,const std::string& id){
            unsigned rows=component.value("rows",4u),cols=component.value("cols",4u);
            bool diodes=component.value("diodes",false);
            label(id+" · 4x4"+(diodes?std::string(" / DIODE"):std::string(" / GHOST")),838,34,p.text2);
            std::set<std::pair<unsigned,unsigned>> closed;
            if(component.contains("closed"))for(const auto& cell:component.at("closed")){
                if(!cell.is_array() || cell.size()!=2)continue;
                closed.insert({unsigned(std::atoi(cell.at(0).get<std::string>().c_str())),unsigned(std::atoi(cell.at(1).get<std::string>().c_str()))});
            }
            for(unsigned row=0;row<rows && row<4;++row)for(unsigned col=0;col<cols && col<4;++col){
                int x=842+int(col)*34,y=52+int(row)*31;bool on=closed.count({row,col})>0;
                g.SetPen(wxPen(on?p.accent:p.stroke,dip(1)));g.SetBrush(wxBrush(on?p.selOnPanel:p.canvas));
                g.DrawRoundedRectangle(dip(x),dip(y),dip(28),dip(25),dip(3));
                label(std::to_string(row)+std::to_string(col),x+8,y+5,on?p.text1:p.text3);
            }
            label("点击切换闭合",842,188,p.text3);
        };
        auto drawOled=[&](const eda::Json& component,const std::string& id){
            unsigned width=component.value("width",64u),height=component.value("height",32u);
            bool on=component.value("display_on",false),inverted=component.value("inverted",false);
            label(id+" · OLED "+std::to_string(width)+"x"+std::to_string(height),990,34,p.text2);
            double scale=std::min(176.0/double(width?width:64u),84.0/double(height?height:32u));
            int cell=int(std::max(1.0,scale));
            if(component.contains("pages")){
                const auto& pages=component.at("pages");
                for(size_t page=0;page<pages.size() && page*8<size_t(height);++page){
                    const auto bytes=pages.at(page).value("bytes",std::string());
                    for(unsigned col=0;col<width && (2*size_t(col)+1)<bytes.size();++col){
                        unsigned value=0;
                        for(int nibble=0;nibble<2;++nibble){
                            char c=bytes[2*size_t(col)+size_t(nibble)];unsigned digit=0;
                            if(c>='0' && c<='9')digit=unsigned(c-'0');
                            else if(c>='a' && c<='f')digit=unsigned(c-'a'+10);
                            else if(c>='A' && c<='F')digit=unsigned(c-'A'+10);
                            value=(value<<4)|digit;
                        }
                        for(unsigned bit=0;bit<8;++bit){
                            bool litPixel=((value>>bit)&1u)!=0;if(inverted)litPixel=!litPixel;
                            auto tone=litPixel?p.warn:p.canvas;
                            g.SetPen(wxPen(tone,dip(1)));g.SetBrush(wxBrush(tone));
                            g.DrawRectangle(dip(990+int(double(col)*scale)),dip(52+int(double(page*8+bit)*scale)),dip(cell),dip(cell));
                        }
                    }
                }
            }
            auto sha=component.value("frame_sha256",std::string());
            label(std::string("CPOL")+std::to_string(component.value("cpol",0u))+" CPHA"+std::to_string(component.value("cpha",0u))
                +(on?std::string(" · ON"):std::string(" · OFF"))+" · lit="+std::to_string(component.value("lit_pixels",0u)),990,142,on?p.text1:p.danger);
            if(sha.size()>=16)label("sha "+sha.substr(0,16),990,160,p.text3);
        };
        auto drawI2c=[&](const eda::Json& component,const std::string& id){
            unsigned address=component.value("address_7bit",0u);
            label(id+" · ADDR 0x"+hexByte(address),990,186,p.text2);
            label("ACK "+std::to_string(component.value("ack_count",0u))+"  NAK "+std::to_string(component.value("nak_count",0u))
                +"  TX "+std::to_string(component.value("transactions",0u)),990,204,
                component.value("nak_count",0u)?p.danger:p.text1);
            label("last 0x"+component.value("last_read_byte",std::string("00"))+" · ptr "+std::to_string(component.value("reg_pointer",0u)),990,222,p.text1);
            label("bus "+component.value("bus_state",std::string("idle")),990,240,p.text3);
        };
        for(const auto& [id,component]:frame.at("board").at("peripherals").items()){
            const auto model=component.value("model",std::string());
            if(model=="sigflow.matrix-keypad@1"){
                advanced=true;drawKeypad(component,id);
            }else if(model=="sigflow.spi-oled@1"){
                advanced=true;drawOled(component,id);
            }else if(model=="sigflow.i2c-sensor@1"){
                advanced=true;drawI2c(component,id);
            }
        }
        if(!advanced)for(const auto& [id,component]:frame.at("board").at("peripherals").items())if(component.value("model",std::string())=="sigflow.seven-segment-mux@1"){
            text(wxString::FromUTF8(id+"  scan="+component.value("scan_period_ticks",std::string("0"))+" ps"),838,36,p.text2);
            for(int digit=0;digit<4;++digit){int dx=846+digit*83,dy=70;int shapes[8][4]={{8,0,38,7},{46,8,7,39},{46,54,7,39},{8,94,38,7},{0,54,7,39},{0,8,7,39},{8,47,38,7},{59,94,8,8}};bool invalid=false;
                for(int segment=0;segment<8;++segment){const auto& observation=component.at("digits").at(digit).at(segment);bool valid=observation.at("instant_valid");invalid|=!valid;unsigned brightness=integrated?observation.at("duty_ppm").get<unsigned>():observation.at("instant_active").get<bool>()?1000000u:0u;
                    auto mix=[&](unsigned off,unsigned on){return uint8_t((uint64_t(off)*(1000000-brightness)+uint64_t(on)*brightness)/1000000);};auto tone=!valid?p.danger:wxColour(mix(p.canvas.Red(),p.warn.Red()),mix(p.canvas.Green(),p.warn.Green()),mix(p.canvas.Blue(),p.warn.Blue()));
                    g.SetPen(wxPen(tone,dip(1)));g.SetBrush(wxBrush(tone));g.DrawRoundedRectangle(dip(dx+shapes[segment][0]),dip(dy+shapes[segment][1]),dip(shapes[segment][2]),dip(shapes[segment][3]),dip(2));}
                if(invalid)text("FLT/CON",dx,180,p.danger);
            }
        }
        int group=0;for(const auto& [id,component]:frame.at("board").at("peripherals").items())if(!advanced && component.value("model",std::string())=="sigflow.dip-switch@1"){
            text(wxString::FromUTF8(id+" / DIP"),838,218+group*65,p.text2);unsigned width=component.at("width"),value=component.at("value");for(unsigned bit=0;bit<width && bit<8;++bit){int x=842+int(bit)*40,y=244+group*65;g.SetPen(wxPen(p.strokeStrong,dip(1)));g.SetBrush(wxBrush(value&(1u<<bit)?p.selOnPanel:p.canvas));g.DrawRoundedRectangle(dip(x),dip(y),dip(30),dip(38),dip(4));text(value&(1u<<bit)?"1":"0",x+9,y+9,p.text1);}++group;
        }
    }
}
int BoardRenderer::HitButton(wxWindow& w,const wxPoint& point,bool phase1){Geometry a(w,phase1);
    for(int i=0;i<2;++i){int x=70+i*155;wxRect rect(int(a.x+w.FromDIP(x-35)*a.scale),int(a.y+w.FromDIP(234)*a.scale),int(w.FromDIP(95)*a.scale),int(w.FromDIP(44)*a.scale));if(rect.Contains(point))return 3+i;}return -1;
}
std::optional<BoardRenderer::KeypadCell> BoardRenderer::HitKeypad(wxWindow& w,const wxPoint& point,const eda::Json& frame){
    if(!frame.value("phase1",false) || !frame.contains("board") || !frame.at("board").contains("peripherals"))return {};
    Geometry geometry(w,true);
    for(const auto& [id,component]:frame.at("board").at("peripherals").items()){
        if(component.value("model",std::string())!="sigflow.matrix-keypad@1")continue;
        unsigned rows=component.value("rows",4u),cols=component.value("cols",4u);
        for(unsigned row=0;row<rows;++row)for(unsigned col=0;col<cols;++col){
            int x=842+int(col)*34,y=52+int(row)*31;
            wxRect rect(int(geometry.x+w.FromDIP(x)*geometry.scale),int(geometry.y+w.FromDIP(y)*geometry.scale),
                int(w.FromDIP(28)*geometry.scale),int(w.FromDIP(25)*geometry.scale));
            if(rect.Contains(point))return KeypadCell{id,row,col};
        }
    }
    return {};
}
std::string BoardRenderer::HitNode(wxWindow& w,const wxPoint& point,bool phase1){Geometry a(w,phase1);auto hit=[&](int x,int y,int width,int height){return wxRect(int(a.x+w.FromDIP(x)*a.scale),int(a.y+w.FromDIP(y)*a.scale),int(w.FromDIP(width)*a.scale),int(w.FromDIP(height)*a.scale)).Contains(point);};for(int i=0;i<6;++i)if(hit(30+i*128,25,95,120))return "led"+std::to_string(i)+".K";for(int i=0;i<2;++i)if(hit(35+i*155,234,95,44))return "key_p"+std::to_string(i+3)+".contact";if(hit(550,235,205,48))return "pad.52";return {};}
std::optional<std::pair<std::string,unsigned>> BoardRenderer::HitDip(wxWindow& w,const wxPoint& point,const eda::Json& frame){
    if(!frame.value("phase1",false) || !frame.contains("board") || !frame.at("board").contains("peripherals"))return {};Geometry geometry(w,true);int group=0;
    for(const auto& [id,component]:frame.at("board").at("peripherals").items())if(component.value("model",std::string())=="sigflow.dip-switch@1"){
        unsigned width=component.at("width");for(unsigned bit=0;bit<width && bit<8;++bit){wxRect rect(int(geometry.x+w.FromDIP(842+int(bit)*40)*geometry.scale),int(geometry.y+w.FromDIP(244+group*65)*geometry.scale),int(w.FromDIP(30)*geometry.scale),int(w.FromDIP(38)*geometry.scale));if(rect.Contains(point))return std::make_pair(id,bit);}++group;
    }return {};
}
}
