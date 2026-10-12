//﻿#include "ToolboxPanel.h"
#include "ToolboxPanel.h"
#include <wx/artprov.h>
#include <wx/dnd.h>
#include <wx/treectrl.h>
#include <wx/textfile.h>
#include <wx/tokenzr.h>
#include <wx/filename.h>
#include <wx/stdpaths.h>
#include <wx/msgdlg.h>        
#include <wx/file.h>
#include <wx/dir.h>
#include <wx/image.h>
#include <map>
#include <wx/arrstr.h>
#include <wx/bmpbndl.h>

#include "platform/PlatformPaths.h"

// T-310：元件库树的字号/配色一律经唯一 Token 层取用（红线 SAFE-012 / THE-001）。
#include "ui/UiTheme.h"

using sigflow::platform::JoinPath;



#define SVG_FOLDER wxT("res/svg/")

namespace {

// T-310 / CMP-035：树项 12 DIP；分组头 10 DIP / Semibold（THE-017 的 DIP 整数档）。
// 分类节点当前是 Input/Output、Wiring、Gates、Block、Definition 五个，按文案判别即可，
// 不需要额外的分类字段（避免触碰树的数据模型）。
wxFont ToolboxItemFont(const wxWindow* window)
{
    return UiTheme::UiFont(window, UiTheme::TextSize::P12, UiTheme::Weight::Normal);
}

wxFont ToolboxGroupFont(const wxWindow* window)
{
    return UiTheme::UiFont(window, UiTheme::TextSize::P10, UiTheme::Weight::Semibold);
}

bool IsToolboxGroupLabel(const wxString& label)
{
    return label == "Input/Output" || label == "Wiring" || label == "Gates" ||
           label == "Block" || label == "Definition";
}

// T-310 / DES-011：分组头大写（wxTreeCtrl 无字距 API，不伪造 .07em 字距）。
wxString GroupHeaderDisplayLabel(const wxString& label)
{
    return IsToolboxGroupLabel(label) ? label.Upper() : label;
}

// 应用整棵树的 chrome 配色（node 现取色 = 0，此前全部走系统默认）。
// 说明：wxTreeCtrl 是原生控件（MSW 仅提供 CDDS_ITEMPREPAINT 逐项定制），
// 没有 hover 态入口，选中底也不可指定为 sel/hover token（见证据文件「未实现」清单），
// 故此处只设置可表达的部分：控件底 surfaceOnPanel、正文 text1、分组头 text3。
void ApplyToolboxTreeLook(wxTreeCtrl* tree)
{
    if (!tree) {
        return;
    }

    const UiTheme::Colours& colours = UiTheme::Palette();
    tree->SetBackgroundColour(colours.surfaceOnPanel);
    tree->SetForegroundColour(colours.text1);
    tree->SetFont(ToolboxItemFont(tree));
    tree->SetIndent(tree->FromDIP(UiTheme::Spacing::S2));  // CMP-035 缩进：8 DIP
}

// CMP-035 的分组头样式：10 DIP / Semibold + text3；文案大写由 Rebuild() 写入。
void ApplyToolboxGroupLook(wxTreeCtrl* tree, const wxTreeItemId& item)
{
    if (!item.IsOk()) {
        return;
    }
    tree->SetItemFont(item, ToolboxGroupFont(tree));
    tree->SetItemTextColour(item, UiTheme::Palette().text3);
}

}  // namespace


static void MY_LOG(const wxString & s)
{
    wxFile f(JoinPath(wxStandardPaths::Get().GetTempDir(), "logsim_tools.log"),
        wxFile::write_append);
    if (f.IsOpened()) {
        f.Write(wxDateTime::Now().FormatISOCombined() + "  " + s + "\n");
        f.Close();
    }
}

// 自定义树项数据
class wxStringTreeItemData : public wxTreeItemData
{
public:
    explicit wxStringTreeItemData(const wxString& s) : m_str(s) {}
    const wxString& GetStr() const { return m_str; }
private:
    wxString m_str;
};

wxBEGIN_EVENT_TABLE(ToolboxPanel, wxPanel)
EVT_TREE_ITEM_ACTIVATED(wxID_ANY, ToolboxPanel::OnItemActivated)
EVT_TREE_BEGIN_DRAG(wxID_ANY, ToolboxPanel::OnBeginDrag)
wxEND_EVENT_TABLE()


ToolboxPanel::ToolboxPanel(wxWindow* parent)
    : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(250, -1))
{
    // 添加 PNG 图像处理器初始化
    //wxImage::AddHandler(new wxPNGHandler);

    // 创建布局
    wxBoxSizer* mainSizer = new wxBoxSizer(wxVERTICAL);

    // 初始化工具树
    m_tree = new wxTreeCtrl(this, wxID_ANY, wxDefaultPosition, wxDefaultSize,
        wxTR_DEFAULT_STYLE | wxTR_HIDE_ROOT | wxTR_FULL_ROW_HIGHLIGHT | wxBORDER_NONE);
    mainSizer->Add(m_tree, 1, wxEXPAND | wxALL, 0);

    SetSizer(mainSizer);

    // 初始化图像列表
    const wxSize iconSize = FromDIP(wxSize(24, 24));
    m_imgList = new wxImageList(iconSize.x, iconSize.y, true, 500);
    const wxBitmap folderFallback = wxArtProvider::GetBitmap(wxART_FOLDER, wxART_OTHER, iconSize);
    wxBitmapBundle folderBundle = wxBitmapBundle::FromSVGFile(
        sigflow::platform::ResourcePath("res\\icons\\project.svg"), iconSize);
    m_imgList->Add(folderBundle.IsOk() ? folderBundle.GetBitmap(iconSize) : folderFallback);

    // 加载所有工具图标
    LoadToolIcon("Wire", "wiring.svg");               // 1: Wiring-Wire
    m_displayToFile["Wire"] = "wiring";
    LoadToolIcon("Splitter", "splitter.svg");         // 2: Wiring-Splitter
    m_displayToFile["Splitter"] = "splitter";
    LoadToolIcon("Pin (Input)", "pinInput.svg");      // 3: Wiring-Pin (Input)
    m_displayToFile["Pin (Input)"] = "pinInput";
    LoadToolIcon("Pin (Output)", "pinOutput.svg");    // 4: Wiring-Pin (Output)
    m_displayToFile["Pin (Output)"] = "pinOutput";
    LoadToolIcon("Probe", "probe.svg");               // 5: Wiring-Probe
    m_displayToFile["Probe"] = "probe";
    LoadToolIcon("Tunnel", "tunnel.svg");             // 6: Wiring-Tunnel
    m_displayToFile["Tunnel"] = "tunnel";
    LoadToolIcon("Pull Resistor", "pullrect.svg");    // 7: Wiring-Pull Resistor
    m_displayToFile["Pull Resistor"] = "pullrect";
    LoadToolIcon("Clock", "clock.svg");               // 8: Wiring-Clock
    m_displayToFile["Clock"] = "clock";
    LoadToolIcon("Constant", "constant.svg");         // 9: Wiring-Constant
    m_displayToFile["Constant"] = "constant";
    LoadToolIcon("Power", "power.svg");               // 10: Wiring-Power
    m_displayToFile["Power"] = "power";
    LoadToolIcon("Ground", "ground.svg");             // 11: Wiring-Ground
    m_displayToFile["Ground"] = "ground";
    LoadToolIcon("Transmission Gate", "transmis.svg");// 12: Wiring-Transmission Gate
    m_displayToFile["Transmission Gate"] = "transmis";
    LoadToolIcon("Bit Extender", "extender.svg");     // 13: Wiring-Bit Extender
    m_displayToFile["Bit Extender"] = "extender";

    // Gates 分类（逻辑门）
    LoadToolIcon("AND Gate", "andGate.svg");          // 15: Gates-AND Gate
    m_displayToFile["AND Gate"] = "and";
    LoadToolIcon("NAND Gate", "nandGate.svg");        // 17: Gates-NAND Gate
    m_displayToFile["NAND Gate"] = "nand";
    LoadToolIcon("OR Gate", "orGate.svg");            // 19: Gates-OR Gate
    m_displayToFile["OR Gate"] = "or";
    LoadToolIcon("NOR Gate", "norGate.svg");          // 21: Gates-NOR Gate
    m_displayToFile["NOR Gate"] = "nor";
    LoadToolIcon("XOR Gate", "xorGate.svg");          // 23: Gates-XOR Gate
    m_displayToFile["XOR Gate"] = "xor";
    LoadToolIcon("XNOR Gate", "xnorGate.svg");        // 25: Gates-XNOR Gate
    m_displayToFile["XNOR Gate"] = "xnor";

    LoadToolIcon("Buffer Gate", "bufferGate.svg");    // 14: Gates-Buffer Gate
    m_displayToFile["Buffer Gate"] = "bufferGate";
    LoadToolIcon("Odd Parity Gate", "parityOddGate.svg");// 27: Gates-Odd Parity Gate
    m_displayToFile["Odd Parity Gate"] = "parityOddGate";
    LoadToolIcon("Even Parity Gate", "parityEvenGate.svg");// 28: Gates-Even Parity Gate
    m_displayToFile["Even Parity Gate"] = "parityEvenGate";
    LoadToolIcon("Controlled Buffer", "controlledBuffer.svg");// 29: Gates-Controlled Buffer
    m_displayToFile["Controlled Buffer"] = "controlledBuffer";
    LoadToolIcon("Controlled Inverter", "controlledInverter.svg");// 30: Gates-Controlled Inverter
    m_displayToFile["Controlled Inverter"] = "controlledInverter";

    // Plexers 分类
    LoadToolIcon("Multiplexer", "multiplexer.svg");   // 31: Plexers-Multiplexer
    m_displayToFile["Multiplexer"] = "multiplexer";
    LoadToolIcon("Demultiplexer", "demultiplexer.svg");// 32: Plexers-Demultiplexer
    m_displayToFile["Demultiplexer"] = "demultiplexer";
    LoadToolIcon("Decoder", "decoder.svg");           // 33: Plexers-Decoder
    m_displayToFile["Decoder"] = "decoder";
    LoadToolIcon("Priority Encoder", "priencod.svg"); // 34: Plexers-Priority Encoder
    m_displayToFile["Priority Encoder"] = "priencod";
    LoadToolIcon("Bit Selector", "bitSelector.svg");  // 35: Plexers-Bit Selector
    m_displayToFile["Bit Selector"] = "bitSelector";

    // Arithmetic 分类
    LoadToolIcon("Adder", "adder.svg");               // 36: Arithmetic-Adder
    m_displayToFile["Adder"] = "adder";
    LoadToolIcon("Subtractor", "subtractor.svg");     // 37: Arithmetic-Subtractor
    m_displayToFile["Subtractor"] = "subtractor";
    LoadToolIcon("Multiplier", "multiplier.svg");     // 38: Arithmetic-Multiplier
    m_displayToFile["Multiplier"] = "multiplier";
    LoadToolIcon("Divider", "divider.svg");           // 39: Arithmetic-Divider
    m_displayToFile["Divider"] = "divider";
    LoadToolIcon("Negator", "negator.svg");           // 40: Arithmetic-Negator
    m_displayToFile["Negator"] = "negator";
    LoadToolIcon("Comparator", "comparator.svg");     // 41: Arithmetic-Comparator
    m_displayToFile["Comparator"] = "comparator";
    LoadToolIcon("Shifter", "shifter.svg");           // 42: Arithmetic-Shifter
    m_displayToFile["Shifter"] = "shifter";
    LoadToolIcon("Bit Adder", "bitadder.svg");        // 43: Arithmetic-Bit Adder
    m_displayToFile["Bit Adder"] = "bitadder";
    LoadToolIcon("Bit Finder", "bitfindr.svg");       // 44: Arithmetic-Bit Finder
    m_displayToFile["Bit Finder"] = "bitfindr";

    // Memory 分类
    LoadToolIcon("D Flip-Flop", "dFlipFlop.svg");     // 45: Memory-D Flip-Flop
    m_displayToFile["D Flip-Flop"] = "dFlipFlop";
    LoadToolIcon("T Flip-Flop", "tFlipFlop.svg");     // 46: Memory-T Flip-Flop
    m_displayToFile["T Flip-Flop"] = "tFlipFlop";
    LoadToolIcon("JK Flip-Flop", "jkFlipFlop.svg");   // 47: Memory-JK Flip-Flop
    m_displayToFile["JK Flip-Flop"] = "jkFlipFlop";
    LoadToolIcon("SR Flip-Flop", "srFlipFlop.svg");   // 48: Memory-SR Flip-Flop
    m_displayToFile["SR Flip-Flop"] = "srFlipFlop";
    LoadToolIcon("Register", "register.svg");         // 49: Memory-Register
    m_displayToFile["Register"] = "register";
    LoadToolIcon("Counter", "counter.svg");           // 50: Memory-Counter
    m_displayToFile["Counter"] = "counter";
    LoadToolIcon("Shift Register", "shiftreg.svg");   // 51: Memory-Shift Register
    m_displayToFile["Shift Register"] = "shiftreg";
    LoadToolIcon("Random Generator", "random.svg");   // 52: Memory-Random Generator
    m_displayToFile["Random Generator"] = "random";
    LoadToolIcon("RAM", "ram.svg");                   // 53: Memory-RAM
    m_displayToFile["RAM"] = "ram";
    LoadToolIcon("ROM", "rom.svg");                   // 54: Memory-ROM
    m_displayToFile["ROM"] = "rom";

    // Input/Output 分类
    LoadToolIcon("Button", "button.svg");             // 55: Input/Output-Button
    m_displayToFile["Button"] = "button";
    LoadToolIcon("Joystick", "joystick.svg");         // 56: Input/Output-Joystick
    m_displayToFile["Joystick"] = "joystick";
    LoadToolIcon("Keyboard", "keyboard.svg");         // 57: Input/Output-Keyboard
    m_displayToFile["Keyboard"] = "keyboard";
    LoadToolIcon("LED", "led.svg");                   // 58: Input/Output-LED
    m_displayToFile["LED"] = "led";
    LoadToolIcon("7-Segment Display", "7seg.svg");    // 59: Input/Output-7-Segment Display
    m_displayToFile["7-Segment Display"] = "7seg";
    LoadToolIcon("Hex Digit Display", "hexdig.svg");  // 60: Input/Output-Hex Digit Display
    m_displayToFile["Hex Digit Display"] = "hexdig";
    LoadToolIcon("LED Matrix", "dotmat.svg");         // 61: Input/Output-LED Matrix
    m_displayToFile["LED Matrix"] = "dotmat";
    LoadToolIcon("TTY", "tty.svg");                   // 62: Input/Output-TTY
    m_displayToFile["TTY"] = "tty";

    // Tools 分类
    LoadToolIcon("Poke Tool", "poke.svg");            // 63: Tools-Poke Tool
    m_displayToFile["Poke Tool"] = "poke";
    LoadToolIcon("Edit Tool", "select.svg");          // 64: Tools-Edit Tool
    m_displayToFile["Edit Tool"] = "select";
    LoadToolIcon("Select Tool", "select.svg");        // 65: Tools-Select Tool
    m_displayToFile["Select Tool"] = "select";
    LoadToolIcon("Wiring Tool", "wiring.svg");        // 66: Tools-Wiring Tool
    m_displayToFile["Wiring Tool"] = "wiring";
    LoadToolIcon("Text Tool", "text.svg");            // 67: Tools-Text Tool
    m_displayToFile["Text Tool"] = "text";
    LoadToolIcon("Menu Tool", "menu.svg");            // 68: Tools-Menu Tool
    m_displayToFile["Menu Tool"] = "menu";
    LoadToolIcon("Label Tool", "text.svg");           // 69: Tools-Label Tool
    m_displayToFile["Label Tool"] = "text";
    LoadToolIcon("BlackBoxDefinition", "blockbox.svg");
   

    // Grammer Block 分类
    LoadToolIcon("Continuous Assign", "continuous_assign.svg");           // 69: Tools-Label Tool
    m_displayToFile["Continuous Assign"] = "continuous_assign";
    LoadToolIcon("Always Block", "always_block.svg");           // 69: Tools-Label Tool
    m_displayToFile["Always Block"] = "always_block";

    m_tree->AssignImageList(m_imgList);
    // T-310 / CMP-035：12 DIP 正文、surfaceOnPanel 底；不再硬编码像素字号与字体族名
    //（PLT-001 尺寸走 FromDIP、PLT-002 字体族取系统 GUI 字体，由 UiTheme::UiFont 保证）。
    ApplyToolboxTreeLook(m_tree);

    // 构建工具树
    Rebuild();

    // T-140 / THE-030：构造末尾建立订阅并立即重施一次（首帧正确），
    // 此后每次主题切换都会按新调色板重取色。回调只做取色 + Refresh(false)。
    m_themeSubscription = UiTheme::Subscribe([this] {
        ReapplyTheme();
        Refresh(false);
    });
    ReapplyTheme();
    //测试函数，测试Definition
    //AddDefinition("TestModule1");
    //AddDefinition("TestModule2");
    //AddDefinition("MyBlackBox");
    // 绑定事件
    m_tree->Bind(wxEVT_TREE_SEL_CHANGED, &ToolboxPanel::OnToolSelected, this);
    // INT-008：每次单击库项都进入预览，包括再次点击已选中的同一项。
    m_tree->Bind(wxEVT_LEFT_DOWN,[this](wxMouseEvent& evt) {
        const auto item=m_tree->HitTest(evt.GetPosition());
        if (item.IsOk() && !m_tree->ItemHasChildren(item)) {
            if (auto* data=dynamic_cast<wxStringTreeItemData*>(m_tree->GetItemData(item))) {
                wxCommandEvent place(wxEVT_COMMAND_MENU_SELECTED,wxID_HIGHEST+900);
                place.SetString(data->GetStr());
                if (auto* target=wxGetTopLevelParent(this)) wxPostEvent(target,place);
            }
        }
        evt.Skip();
    });
}

// T-140 / THE-030：面板级主题重施入口。
// 逐项重取 UiTheme token 并写回**已存在**的控件；不做任何重布局（不调 Layout / SetSize /
// SendSizeEvent），不改树的数据模型与事件绑定，不重建任何树项。
void ToolboxPanel::ReapplyTheme()
{
    ApplyToolboxTreeLook(m_tree);

    // 分组头（10 DIP / Semibold + text3）此前只在构造期 Rebuild() 里逐个写入一次；
    // 这里对记下的 5 个分类头重施。只读取已有句柄，不新增/删除树项。
    for (const wxTreeItemId& item : m_groupItems) {
        ApplyToolboxGroupLook(m_tree, item);
    }
}

void ToolboxPanel::LoadToolIcon(const wxString& toolName, const wxString& svgFileName)
{
    // 用 ResourcePath 定位（相对 **可执行文件目录**），不要用相对 CWD 的裸路径。
    // 旧实现是 wxString fullPath = SVG_FOLDER + svgFileName（"res/svg/..."）：
    // 从桌面项/快捷方式/其它工作目录启动时，wxFileExists 直接为 false，
    // 结果是**所有工具箱图标静默消失**。ResourcePath 还会把 '\' 规范化为 '/'。
    const wxString fullPath =
        sigflow::platform::ResourcePath(SVG_FOLDER + svgFileName);

    if (!wxFileExists(fullPath)) {
        return;
    }

    // 先渲染为较大的尺寸，避免被裁剪
    const int targetSize = FromDIP(24);
    const int renderSize = 64;  // 先大尺寸渲染

    wxBitmapBundle bundle = wxBitmapBundle::FromSVGFile(fullPath, wxSize(renderSize, renderSize));

    if (!bundle.IsOk()) {
        //MY_LOG("❌ SVG 加载失败：" + toolName);
        return;
    }

    wxBitmap largeBmp = bundle.GetBitmap(wxSize(renderSize, renderSize));

    if (!largeBmp.IsOk()) {
        //MY_LOG("❌ SVG 生成大尺寸 Bitmap 失败：" + toolName);
        return;
    }

    wxImage img = largeBmp.ConvertToImage();
    if (!img.IsOk() || img.GetWidth() <= 0 || img.GetHeight() <= 0) {
        return;
    }

    // 计算等比例缩放
    int w = img.GetWidth();
    int h = img.GetHeight();

    double scale = std::min(
        (double)targetSize / w,
        (double)targetSize / h
    );

    int newW = (int)(w * scale);
    int newH = (int)(h * scale);
    if (newW < 1) newW = 1;
    if (newH < 1) newH = 1;

    img = img.Scale(newW, newH, wxIMAGE_QUALITY_HIGH);

    // 创建 24x24 的**透明**画布，再把缩放后的图标居中贴上去。
    //
    // 旧实现用 wxMemoryDC + *wxWHITE_BRUSH 清背景，把注释里声称的"透明背景"
    // 实际画成了不透明白块 —— 在暗色主题下每个工具图标都是一个白色方块。
    wxImage canvas(targetSize, targetSize);
    canvas.InitAlpha();
    unsigned char* alpha = canvas.GetAlpha();
    const std::size_t pixelCount = static_cast<std::size_t>(targetSize) * targetSize;
    for (std::size_t i = 0; i < pixelCount; ++i) {
        alpha[i] = 0;   // 全透明
    }
    if (!img.HasAlpha()) {
        img.InitAlpha();   // 源图没有 alpha 时按"完全不透明"参与混合
    }

    const int offsetX = (targetSize - newW) / 2;
    const int offsetY = (targetSize - newH) / 2;
    canvas.Paste(img, offsetX, offsetY, wxIMAGE_ALPHA_BLEND_OVER);

    wxBitmap finalBmp(canvas);
    if (!finalBmp.IsOk()) {
        return;
    }

    int iconIndex = m_imgList->Add(finalBmp);
    m_toolIconIndex[toolName] = iconIndex;
}



// 构建工具树
void ToolboxPanel::Rebuild()
{
    m_tree->DeleteAllItems();
    // T-140：树已被清空，之前记录的分类头句柄（id 即指针）全部失效，必须一并清掉，
    // 否则主题重施会拿悬空 id 去 SetItemTextColour。
    m_groupItems.clear();
    wxTreeItemId root = m_tree->AddRoot("Logisim Tools", 0, 0);

    // ========== Input/Output 分类 ==========
    wxArrayString ioDisplayNames;
    ioDisplayNames.Add("Pin (Input)");
    ioDisplayNames.Add("Pin (Output)");
    ioDisplayNames.Add("Clock");
    ioDisplayNames.Add("Power");
    ioDisplayNames.Add("Ground");
    ioDisplayNames.Add("Constant");
    ioDisplayNames.Add("Button");
    ioDisplayNames.Add("Joystick");
    ioDisplayNames.Add("Keyboard");
    ioDisplayNames.Add("LED");
    ioDisplayNames.Add("7-Segment Display");
    ioDisplayNames.Add("Hex Digit Display");
    ioDisplayNames.Add("LED Matrix");
    ioDisplayNames.Add("TTY");

    wxTreeItemId ioId = m_tree->AppendItem(root, GroupHeaderDisplayLabel("Input/Output"), 0, 0);
    m_tree->SetItemBold(ioId, true);
    ApplyToolboxGroupLook(m_tree, ioId);
    m_groupItems.push_back(ioId);  // T-140：登记分类头，供主题切换时重施
    for (const auto& displayName : ioDisplayNames) {
        wxString fileName = m_displayToFile[displayName];
        int iconIdx = GetToolIconIndex(displayName);
        m_tree->AppendItem(ioId, displayName, iconIdx, iconIdx,
            new wxStringTreeItemData(fileName));
    }

    // ========== Wiring 分类 ==========
    wxArrayString wiringDisplayNames;
    wiringDisplayNames.Add("Splitter");
    wiringDisplayNames.Add("Probe");
    wiringDisplayNames.Add("Tunnel");
    wiringDisplayNames.Add("Pull Resistor");
    wiringDisplayNames.Add("Transmission Gate");
    wiringDisplayNames.Add("Bit Extender");

    wxTreeItemId wiringId = m_tree->AppendItem(root, GroupHeaderDisplayLabel("Wiring"), 0, 0);
    m_tree->SetItemBold(wiringId, true);
    ApplyToolboxGroupLook(m_tree, wiringId);
    m_groupItems.push_back(wiringId);  // T-140
    for (const auto& displayName : wiringDisplayNames) {
        wxString fileName = m_displayToFile[displayName];
        int iconIdx = GetToolIconIndex(displayName);
        m_tree->AppendItem(wiringId, displayName, iconIdx, iconIdx,
            new wxStringTreeItemData(fileName));
    }

    // ========== Gates 分类 ==========
    wxArrayString gatesDisplayNames;
    //gatesDisplayNames.Add("Buffer Gate");
    gatesDisplayNames.Add("AND Gate");
    //gatesDisplayNames.Add("AND Gate (Rect)");
    gatesDisplayNames.Add("NAND Gate");
    //gatesDisplayNames.Add("NAND Gate (Rect)");
    gatesDisplayNames.Add("OR Gate");
    //gatesDisplayNames.Add("OR Gate (Rect)");
    gatesDisplayNames.Add("NOR Gate");
    //gatesDisplayNames.Add("NOR Gate (Rect)");
    gatesDisplayNames.Add("XOR Gate");
    //gatesDisplayNames.Add("XOR Gate (Rect)");
    gatesDisplayNames.Add("XNOR Gate");
    //gatesDisplayNames.Add("XNOR Gate (Rect)");
    //gatesDisplayNames.Add("Odd Parity Gate");
    //gatesDisplayNames.Add("Even Parity Gate");
    //gatesDisplayNames.Add("Controlled Buffer");
    //gatesDisplayNames.Add("Controlled Inverter");

    wxTreeItemId gatesId = m_tree->AppendItem(root, GroupHeaderDisplayLabel("Gates"), 0, 0);
    m_tree->SetItemBold(gatesId, true);
    ApplyToolboxGroupLook(m_tree, gatesId);
    m_groupItems.push_back(gatesId);  // T-140
    for (const auto& displayName : gatesDisplayNames) {
        wxString fileName = m_displayToFile[displayName];
        int iconIdx = GetToolIconIndex(displayName);
        m_tree->AppendItem(gatesId, displayName, iconIdx, iconIdx,
            new wxStringTreeItemData(fileName));
    }

    // ==========  Block 分类 ==========
    wxArrayString blockDisplayNames;
    blockDisplayNames.Add("Continuous Assign");
    blockDisplayNames.Add("Always Block");


    wxTreeItemId blockId = m_tree->AppendItem(root, GroupHeaderDisplayLabel("Block"), 0, 0);
    m_tree->SetItemBold(blockId, true);
    ApplyToolboxGroupLook(m_tree, blockId);
    m_groupItems.push_back(blockId);  // T-140
    for (const auto& displayName : blockDisplayNames) {
        wxString fileName = m_displayToFile[displayName];
        int iconIdx = GetToolIconIndex(displayName);
        m_tree->AppendItem(blockId, displayName, iconIdx, iconIdx,
            new wxStringTreeItemData(fileName));
    }


    // ==========  Definition 分类 ==========
    wxArrayString defDisplayNames;


    wxTreeItemId defId = m_tree->AppendItem(root, GroupHeaderDisplayLabel("Definition"), 0, 0);
    m_def = defId;
    m_tree->SetItemBold(defId, true);
    ApplyToolboxGroupLook(m_tree, defId);
    m_groupItems.push_back(defId);  // T-140
    for (const auto& displayName : defDisplayNames) {
        wxString fileName = m_displayToFile[displayName];
        int iconIdx = GetToolIconIndex(displayName);
        m_tree->AppendItem(defId, displayName, iconIdx, iconIdx,
            new wxStringTreeItemData(fileName));
    }

    /*
    // ========== Plexers 分类 ==========
    wxArrayString plexersDisplayNames;
    plexersDisplayNames.Add("Multiplexer");
    plexersDisplayNames.Add("Demultiplexer");
    plexersDisplayNames.Add("Decoder");
    plexersDisplayNames.Add("Priority Encoder");
    plexersDisplayNames.Add("Bit Selector");

    wxTreeItemId plexersId = m_tree->AppendItem(root, "Plexers", 0, 0);
    m_tree->SetItemBold(plexersId, true);
    for (const auto& displayName : plexersDisplayNames) {
        wxString fileName = m_displayToFile[displayName];
        int iconIdx = GetToolIconIndex(displayName);
        m_tree->AppendItem(plexersId, displayName, iconIdx, iconIdx,
            new wxStringTreeItemData(fileName));
    }

    // ========== Arithmetic 分类 ==========
    wxArrayString arithmeticDisplayNames;
    arithmeticDisplayNames.Add("Adder");
    arithmeticDisplayNames.Add("Subtractor");
    arithmeticDisplayNames.Add("Multiplier");
    arithmeticDisplayNames.Add("Divider");
    arithmeticDisplayNames.Add("Negator");
    arithmeticDisplayNames.Add("Comparator");
    arithmeticDisplayNames.Add("Shifter");
    arithmeticDisplayNames.Add("Bit Adder");
    arithmeticDisplayNames.Add("Bit Finder");

    wxTreeItemId arithmeticId = m_tree->AppendItem(root, "Arithmetic", 0, 0);
    m_tree->SetItemBold(arithmeticId, true);
    for (const auto& displayName : arithmeticDisplayNames) {
        wxString fileName = m_displayToFile[displayName];
        int iconIdx = GetToolIconIndex(displayName);
        m_tree->AppendItem(arithmeticId, displayName, iconIdx, iconIdx,
            new wxStringTreeItemData(fileName));
    }

    // ========== Memory 分类 ==========
    wxArrayString memoryDisplayNames;
    memoryDisplayNames.Add("D Flip-Flop");
    memoryDisplayNames.Add("T Flip-Flop");
    memoryDisplayNames.Add("JK Flip-Flop");
    memoryDisplayNames.Add("SR Flip-Flop");
    memoryDisplayNames.Add("Register");
    memoryDisplayNames.Add("Counter");
    memoryDisplayNames.Add("Shift Register");
    memoryDisplayNames.Add("Random Generator");
    memoryDisplayNames.Add("RAM");
    memoryDisplayNames.Add("ROM");

    wxTreeItemId memoryId = m_tree->AppendItem(root, "Memory", 0, 0);
    m_tree->SetItemBold(memoryId, true);
    for (const auto& displayName : memoryDisplayNames) {
        wxString fileName = m_displayToFile[displayName];
        int iconIdx = GetToolIconIndex(displayName);
        m_tree->AppendItem(memoryId, displayName, iconIdx, iconIdx,
            new wxStringTreeItemData(fileName));
    }*/

    /*

    // ========== Tools 分类 ==========
    wxArrayString toolsDisplayNames;
    toolsDisplayNames.Add("Poke Tool");
    toolsDisplayNames.Add("Edit Tool");
    toolsDisplayNames.Add("Select Tool");
    toolsDisplayNames.Add("Wiring Tool");
    toolsDisplayNames.Add("Text Tool");
    toolsDisplayNames.Add("Menu Tool");
    toolsDisplayNames.Add("Label Tool");

    wxTreeItemId toolsId = m_tree->AppendItem(root, "Tools", 0, 0);
    m_tree->SetItemBold(toolsId, true);
    for (const auto& displayName : toolsDisplayNames) {
        wxString fileName = m_displayToFile[displayName];
        int iconIdx = GetToolIconIndex(displayName);
        m_tree->AppendItem(toolsId, displayName, iconIdx, iconIdx,
            new wxStringTreeItemData(fileName));
    }
    */
    // 展开所有分类
    wxTreeItemIdValue cookie;
    wxTreeItemId child = m_tree->GetFirstChild(root, cookie);
    while (child.IsOk()) {
        m_tree->Expand(child);
        child = m_tree->GetNextChild(root, cookie);
    }
}

// 工具名 → 图标索引映射
int ToolboxPanel::GetToolIconIndex(const wxString& toolName)
{
    //static std::map<wxString, int> toolToIconMap = {
    //    // 1. Wiring 分类（索引1-13）
    //    {"Wire", 1},
    //    {"Splitter", 2},
    //    {"Pin (Input)", 3},
    //    {"Pin (Output)", 4},
    //    {"Probe", 5},
    //    {"Tunnel", 6},
    //    {"Pull Resistor", 7},
    //    {"Clock", 8},
    //    {"Constant", 9},
    //    {"Power", 10},
    //    {"Ground", 11},
    //    {"Transmission Gate", 12},
    //    {"Bit Extender", 13},

    //    // 2. Gates 分类（索引14-30）
    //    {"Buffer Gate", 14},
    //    {"AND Gate", 15},
    //    {"AND Gate (Rect)", 16},
    //    {"NAND Gate", 17},
    //    {"NAND Gate (Rect)", 18},
    //    {"OR Gate", 19},
    //    {"OR Gate (Rect)", 20},
    //    {"NOR Gate", 21},
    //    {"NOR Gate (Rect)", 22},
    //    {"XOR Gate", 23},
    //    {"XOR Gate (Rect)", 24},
    //    {"XNOR Gate", 25},
    //    {"XNOR Gate (Rect)", 26},
    //    {"Odd Parity Gate", 27},
    //    {"Even Parity Gate", 28},
    //    {"Controlled Buffer", 29},
    //    {"Controlled Inverter", 30},

    //    // 3. Plexers 分类（索引31-35）
    //    {"Multiplexer", 31},
    //    {"Demultiplexer", 32},
    //    {"Decoder", 33},
    //    {"Priority Encoder", 34},
    //    {"Bit Selector", 35},

    //    // 4. Arithmetic 分类（索引36-44）
    //    {"Adder", 36},
    //    {"Subtractor", 37},
    //    {"Multiplier", 38},
    //    {"Divider", 39},
    //    {"Negator", 40},
    //    {"Comparator", 41},
    //    {"Shifter", 42},
    //    {"Bit Adder", 43},
    //    {"Bit Finder", 44},

    //    // 5. Memory 分类（索引45-54）
    //    {"D Flip-Flop", 45},
    //    {"T Flip-Flop", 46},
    //    {"JK Flip-Flop", 47},
    //    {"SR Flip-Flop", 48},
    //    {"Register", 49},
    //    {"Counter", 50},
    //    {"Shift Register", 51},
    //    {"Random Generator", 52},
    //    {"RAM", 53},
    //    {"ROM", 54},

    //    // 6. Input/Output 分类（索引55-62）
    //    {"Button", 55},
    //    {"Joystick", 56},
    //    {"Keyboard", 57},
    //    {"LED", 58},
    //    {"7-Segment Display", 59},
    //    {"Hex Digit Display", 60},
    //    {"LED Matrix", 61},
    //    {"TTY", 62},

    //    // 7. Tools 分类（索引63-69）
    //    {"Poke Tool", 63},
    //    {"Edit Tool", 64},
    //    {"Select Tool", 65},
    //    {"Wiring Tool", 66},
    //    {"Text Tool", 67},
    //    {"Menu Tool", 68},
    //    {"Label Tool", 69}
    //};
    // 找不到对应工具时，默认用Wire的图标（避免空白）
    auto it = m_toolIconIndex.find(toolName);
    if (it != m_toolIconIndex.end())
        return it->second;
    return -1; // 无图标（树中该项将不显示图标）
}

// 工具激活事件（双击）
void ToolboxPanel::OnItemActivated(wxTreeEvent& evt)
{
    wxTreeItemId item = evt.GetItem();
    wxStringTreeItemData* data = dynamic_cast<wxStringTreeItemData*>(m_tree->GetItemData(item));
    if (data) {
        wxString fileName = data->GetStr();
        wxCommandEvent cmdEvt(wxEVT_COMMAND_MENU_SELECTED, wxID_HIGHEST + 900);
        cmdEvt.SetString(fileName);
        // 关闭期间顶层窗口可能已不存在，不能向空目标投递异步事件。
        if (wxWindow* destination = wxGetTopLevelParent(this)) {
            wxPostEvent(destination, cmdEvt);
        }
    }
}

// 开始拖拽事件
void ToolboxPanel::OnBeginDrag(wxTreeEvent& evt)
{
    wxTreeItemId item = evt.GetItem();
    wxStringTreeItemData* data = dynamic_cast<wxStringTreeItemData*>(m_tree->GetItemData(item));
    if (data) {
        wxString fileName = data->GetStr();
        wxTextDataObject dragData(fileName);
        wxDropSource source(dragData, this);
        source.DoDragDrop(wxDrag_CopyOnly);
    }
}

// 工具选中事件（单击）
void ToolboxPanel::OnToolSelected(wxTreeEvent& evt)
{
    wxTreeItemId selectedItem = evt.GetItem();
    if (!m_tree->ItemHasChildren(selectedItem)) {
        wxString toolName = m_tree->GetItemText(selectedItem); // 显示名称
        wxCommandEvent propEvent(wxEVT_COMMAND_MENU_SELECTED, wxID_HIGHEST + 901);
        propEvent.SetString(toolName);
        if (wxWindow* destination = wxGetTopLevelParent(this)) {
            wxPostEvent(destination, propEvent);
        }
    }
    else {
        wxCommandEvent propEvent(wxEVT_COMMAND_MENU_SELECTED, wxID_HIGHEST + 901);
        propEvent.SetString("");
        if (wxWindow* destination = wxGetTopLevelParent(this)) {
            wxPostEvent(destination, propEvent);
        }
    }
}


void ToolboxPanel::AddDefinition(wxString defId) {
    if (!m_def.IsOk()) return;

    int iconIdx = GetToolIconIndex("BlackBoxDefinition");
    if (iconIdx == -1) iconIdx = 0;
    // 1. 添加子项
    m_tree->AppendItem(m_def, defId, iconIdx, iconIdx, new wxStringTreeItemData(defId));

    // 2. 展开父节点 m_def
    m_tree->Expand(m_def); 
}


void ToolboxPanel::DelDefinition(wxString defId) {
    if (!m_def.IsOk()) return; // m_def 节点不存在

    wxTreeItemIdValue cookie;
    wxTreeItemId child = m_tree->GetFirstChild(m_def, cookie);

    while (child.IsOk()) {
        if (m_tree->GetItemText(child) == defId) {
            m_tree->Delete(child);
            return; // 找到并删除后退出
        }
        child = m_tree->GetNextChild(m_def, cookie);
    }
}
