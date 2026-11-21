#pragma once
#include <eda/api/Types.h>
#include <wx/graphics.h>
#include <wx/window.h>
#include <map>
#include <optional>
namespace sigflow::virtual_lab {
enum class NodeVisual { Low,High,Floating,Contention,Unknown };
NodeVisual ClassifyNode(const eda::Json&);
class BoardRenderer {
public:
    struct SlotStatus {
        bool mounted = false;
        bool multiplexed = false;
        std::string label;
        std::string state_text;
    };
    static SlotStatus LedSlotStatus(const eda::Json&,int index);
    static SlotStatus KeySlotStatus(const eda::Json&,int pad);
    static void Paint(wxWindow&,wxGraphicsContext&,const eda::Json&,const std::map<int,std::string>&,bool integrated=false);
    static int HitButton(wxWindow&,const wxPoint&,bool phase1=false);
    static std::optional<std::pair<std::string,unsigned>> HitDip(wxWindow&,const wxPoint&,const eda::Json&);
    // Matrix keypad cell hit test for the Phase 3/4 teaching board.
    struct KeypadCell { std::string component; unsigned row=0,col=0; };
    static std::optional<KeypadCell> HitKeypad(wxWindow&,const wxPoint&,const eda::Json&);
    static std::string HitNode(wxWindow&,const wxPoint&,bool phase1);
};
}
