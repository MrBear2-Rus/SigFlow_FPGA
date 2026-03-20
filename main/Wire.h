#pragma once
#include <wx/wx.h>
#include <vector>
#include <tuple>

class CanvasPanel;
class SignalNode;

struct WireAnchor {
    size_t wireIdx;   
    size_t ptIdx;     
    bool   isInput;   
    size_t pinIdx;    
    wxPoint oldPos;   
};
enum class CPType { Pin, Bend, Free, Branch };
enum class CellType { Mid, Norm };
enum LogicSignal {
    ZERO,
    ONE,
    X,
    Z,
    E
};

struct ControlPoint {
    wxPoint  pos;
    CPType   type = CPType::Free;
    // 若吸附到引脚，可扩展存元件指针/pin索引
};

struct Cell {
    wxPoint pos;
    CellType type;
    int pre_pts_idx;
    int next_pts_idx;
};

struct Endpoint {
    int elemIdx = -1;
    int wireIdx = -1;
    bool isInput = false;
    int PinIdx = -1;
};

class Wire {
public:
    std::vector<ControlPoint> pts;
    Endpoint Left;
    Endpoint Right;
    
    Wire() = default;
    explicit Wire(std::vector<ControlPoint> v) : pts(std::move(v)) {}

    wxFont font = wxFont(5, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_BOLD);
    //显式声明拷贝/移动构造函数和赋值运算符
    Wire(const Wire&) = default;
    Wire(Wire&&) = default;
    Wire& operator=(const Wire&) = default;
    Wire& operator=(Wire&&) = default;

    // 核心接口
    void Draw(wxGraphicsContext* gc) const;                          // 画线
    void DrawColor(wxGraphicsContext* gc) const;
    void AddPoint(const ControlPoint& cp) { pts.push_back(cp); }
    void Clear() { pts.clear(); }
    bool Empty() const { return pts.empty(); }
    size_t Size() const { return pts.size(); }

    wxRect GetBounds() const;

    CanvasPanel* m_canvas;
    std::vector<Cell> cells;          // 每 2 px 小格中心
    void GenerateCells();                // 一次性切分

    void SetSelf(SignalNode* s);
    SignalNode* GetSelf() const { return self; }
    
    std::vector<wxColor> colors = { wxColour(0, 128, 0), wxColour(0, 255, 0), *wxBLUE };         // 颜色序列:导线0颜色，导线1颜色，导线分支点颜色, 自由点颜色

public:
    // ... 现有成员 ...

    // 分支相关
    LogicSignal status;
    static std::vector<ControlPoint> Route(const ControlPoint& start, const ControlPoint& end);
    void SetStatus(LogicSignal s) { status = s; };

private :
    SignalNode* self = nullptr;  // 关联的信号节点指针
    wxString identifier;
};
