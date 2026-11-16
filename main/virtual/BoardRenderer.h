#pragma once
#include <eda/api/Types.h>
#include <wx/graphics.h>
#include <wx/window.h>
#include <map>
namespace sigflow::virtual_lab {
enum class NodeVisual { Low,High,Floating,Contention,Unknown };
NodeVisual ClassifyNode(const eda::Json&);
class BoardRenderer {
public:
    static void Paint(wxWindow&,wxGraphicsContext&,const eda::Json&,const std::map<int,std::string>&);
    static int HitButton(wxWindow&,const wxPoint&);
};
}
