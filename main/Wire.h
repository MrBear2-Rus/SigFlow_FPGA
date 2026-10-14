#pragma once
#include <wx/wx.h>
#include <vector>
#include <tuple>

#include "ui/UiTheme.h"   // T-410：画布唯一取色来源（SPEC SAFE-012 / THE-001）

class CanvasPanel;
class SignalNode;

// ---------------------------------------------------------------------------
// T-410 画布绘制常量（SPEC CMP-041…CMP-046）
//
// 选中/导线取色在绘制时读取 UiTheme，避免静态缓存与主题变化脱节。
// 尺寸一律 DIP（PLT-001）；颜色一律来自 UiTheme，禁止新增颜色字面量。
// ---------------------------------------------------------------------------

// 尺寸常量（SPEC CMP-041/CMP-044/DES-007）
namespace CanvasDraw {
inline constexpr double kWireWidth      = 2.0;   ///< 导线线宽 2 DIP（CMP-044）
inline constexpr double kSelectionWidth = 1.6;   ///< 选中描边 1.6 DIP（CMP-042、DES-007 例外）
inline constexpr double kNodeRadius     = 3.6;   ///< 结点半径 3.6 DIP 实心（CMP-044）
inline constexpr int    kGridFine       = 20;     ///< 细网格 20 DIP（CMP-041）
inline constexpr int    kGridCoarse     = 100;    ///< 粗网格 100 DIP（CMP-041）
}  // namespace CanvasDraw

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

    wxFont font = UiTheme::MonoFont(nullptr, UiTheme::TextSize::P11);
    //显式声明拷贝/移动构造函数和赋值运算符
    Wire(const Wire&) = default;
    Wire(Wire&&) = default;
    Wire& operator=(const Wire&) = default;
    Wire& operator=(Wire&&) = default;

    // 核心接口
    void Draw(wxGraphicsContext* gc, double screenDip = 1.0, bool selected = false) const;
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
    
    // 颜色序列: [0] 导线常态色，[1] 导通/高亮态导线色，[2] 分支点色
    // T-410：原字面量 { wxColour(0,128,0), wxColour(0,255,0), *wxBLUE } →
    // 全部改为 token：常态导线 = wire（CMP-044），导通态 = brand（语义绿，THE-034 clk 同族），
    // 分支点 = wire（与导线同色，保持"结点属于导线"的语义）。
    std::vector<wxColor> colors = { UiTheme::Palette().wire,
                                    UiTheme::Palette().brand,
                                    UiTheme::Palette().wire };

public:
    // ... 现有成员 ...

    // 分支相关
    LogicSignal status = LogicSignal::ZERO;
    static std::vector<ControlPoint> Route(const ControlPoint& start, const ControlPoint& end);
    void SetStatus(LogicSignal s) { status = s; };

private :
    SignalNode* self = nullptr;  // 关联的信号节点指针
    wxString identifier;
};
