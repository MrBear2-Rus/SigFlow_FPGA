#include "CanvasTextElement.h"
#include "CanvasPanel.h"

CanvasTextElement::CanvasTextElement(CanvasPanel* parent, const wxString& text, const wxPoint& pos)
    : m_parent(parent), m_text(text), m_position(pos), m_size(100, 30),
    m_editing(false), m_hiddenTextCtrl(nullptr) {
    UpdateSize();
}

CanvasTextElement::~CanvasTextElement() {
    DetachHiddenTextCtrl();
}

void CanvasTextElement::AttachHiddenTextCtrl(wxTextCtrl* hiddenCtrl) {
    m_hiddenTextCtrl = hiddenCtrl;
    if (m_hiddenTextCtrl && m_editing) {
        SyncToHiddenCtrl();
        UpdateHiddenTextCtrlPosition();
    }
}

void CanvasTextElement::DetachHiddenTextCtrl() {
    if (m_hiddenTextCtrl && m_editing) {
        SyncFromHiddenCtrl();
    }
    m_hiddenTextCtrl = nullptr;
}

void CanvasTextElement::UpdateHiddenTextCtrlPosition() {
    if (m_hiddenTextCtrl && m_editing) {
        // 将隐藏TextCtrl移动到CanvasTextElement的位置
        wxPoint screenPos = m_parent->CanvasToScreen(m_position);
        wxSize screenSize = wxSize(
            static_cast<int>(m_size.x * m_parent->GetScale()),
            static_cast<int>(m_size.y * m_parent->GetScale())
        );

        m_hiddenTextCtrl->SetPosition(screenPos);
        m_hiddenTextCtrl->SetSize(screenSize);
    }
}

void CanvasTextElement::StartEditing() {
    m_editing = true;

    if (m_hiddenTextCtrl) {
        SyncToHiddenCtrl();
        UpdateHiddenTextCtrlPosition();

        // 设置隐藏TextCtrl的样式
        m_hiddenTextCtrl->SetFont(GetModernFont());
        m_hiddenTextCtrl->SetForegroundColour(wxColour(60, 60, 60));
        m_hiddenTextCtrl->SetFocus();
        m_hiddenTextCtrl->SelectAll();
    }
}

void CanvasTextElement::StopEditing() {
    if (m_editing && m_hiddenTextCtrl) {
        SyncFromHiddenCtrl();
    }
    m_editing = false;
}

void CanvasTextElement::SyncToHiddenCtrl() {
    if (m_hiddenTextCtrl) {
        m_hiddenTextCtrl->SetValue(m_text);
    }
}

void CanvasTextElement::SyncFromHiddenCtrl() {
    if (m_hiddenTextCtrl) {
        m_text = m_hiddenTextCtrl->GetValue();
        UpdateSize();
    }
}

// 绘制方法保持不变
void CanvasTextElement::Draw(wxGraphicsContext* gc) {
    if (m_editing) {
        DrawEditingState(gc);
    }
    else {
        DrawNormalState(gc);
    }
}

void CanvasTextElement::DrawNormalState(wxGraphicsContext* gc) {
    if (!gc) return;

    // 1. 设置画笔：灰色，线宽 3
    gc->SetPen(wxPen(wxColour(*wxLIGHT_GREY), 3));
    gc->SetBrush(*wxTRANSPARENT_BRUSH);

    // 2. 绘制矩形（使用逻辑坐标，精确控制 1px 的偏移）
    // 注意：GC 的 DrawRectangle 参数是 (x, y, w, h)
    gc->DrawRectangle(m_position.x - 1, m_position.y - 1, m_size.x + 2, m_size.y + 2);

    // 3. 绘制文字（内部也需改为接受 gc）
    DrawTextContent(gc);
}

void CanvasTextElement::DrawEditingState(wxGraphicsContext* gc) {
    if (!gc) return;

    // 1. 绘制黑色边框
    gc->SetPen(wxPen(wxColour(*wxBLACK), 3));
    gc->SetBrush(*wxTRANSPARENT_BRUSH);
    gc->DrawRectangle(m_position.x - 1, m_position.y - 1, m_size.x + 2, m_size.y + 2);

    DrawTextContent(gc);

    // 2. 绘制光标
    if (m_editing) {
        // 在 GC 中，使用 GetTextExtent 获取尺寸
        double textW, textH, descent, externalLeading;
        gc->GetTextExtent(m_text, &textW, &textH, &descent, &externalLeading);

        // 这里的 8 是你代码里的偏移量，建议也加上 FromDIP(8) 做适配
        double cursorX = m_position.x + 8 + textW;
        double cursorY1 = m_position.y + 5;
        double cursorY2 = m_position.y + m_size.y - 5;

        // 绘制光标线（用 StrokeLine）
        gc->SetPen(wxPen(*wxBLACK, 1));
        gc->StrokeLine(cursorX, cursorY1, cursorX, cursorY2);
    }
}

void CanvasTextElement::DrawTextContent(wxGraphicsContext* gc) {
    if (!gc) return;

    // 1. 创建并设置字体
    // 注意：gc->CreateFont 将字体和颜色打包在一起
    wxGraphicsFont gcFont = gc->CreateFont(GetModernFont(), wxColour(60, 60, 60));
    gc->SetFont(gcFont);

    // 2. 获取精确的文字尺寸（浮点数）
    double textW, textH, descent, externalLeading;
    gc->GetTextExtent(m_text, &textW, &textH, &descent, &externalLeading);

    // 3. 计算垂直居中位置 (使用 double 避免整数除法的舍入误差)
    double textY = m_position.y + (m_size.y - textH) / 2.0;

    // 4. 绘制文字
    wxGraphicsFont gFont = gc->CreateFont(wxFont(10, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_NORMAL), *wxBLACK);

    // 2. 必须显式设置给 gc
    gc->SetFont(gFont);
    gc->DrawText(m_text, m_position.x + 8, textY);
}

void CanvasTextElement::DrawRoundedRect(wxGraphicsContext* gc, const wxRect& rect, double radius) {
    if (!gc) return;

    // 直接使用 gc 的圆角矩形接口
    // 参数：x, y, width, height, radius
    gc->DrawRoundedRectangle(rect.x, rect.y, rect.width, rect.height, radius);
}
wxFont CanvasTextElement::GetModernFont() {
    return wxFont(11, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
        wxFONTWEIGHT_NORMAL, false, "Segoe UI");
}

void CanvasTextElement::UpdateSize() {
    wxClientDC dc(wxTheApp->GetTopWindow());
    dc.SetFont(GetModernFont());
    wxSize textSize = dc.GetTextExtent(m_text);
    m_size.x = textSize.x + 20;
    m_size.y = wxMax(30, textSize.y + 10);

    // 更新隐藏TextCtrl的位置
    UpdateHiddenTextCtrlPosition();
}

bool CanvasTextElement::Contains(const wxPoint& point) const {
    return wxRect(m_position, m_size).Contains(point);
}

void CanvasTextElement::SetText(const wxString& text) {
    m_text = text;
    UpdateSize();
    if (m_hiddenTextCtrl && m_editing) {
        SyncToHiddenCtrl();
    }
}

wxString CanvasTextElement::GetText() const {
    return m_text;
}

void CanvasTextElement::SetPosition(const wxPoint& pos) {
    m_position = pos;
    UpdateHiddenTextCtrlPosition();
}

void CanvasTextElement::OnTextChanged(const wxString& newText) {
    m_text = newText;
    UpdateSize();

    // 如果附加了隐藏TextCtrl，同步文本
    if (m_hiddenTextCtrl && m_editing) {
        m_hiddenTextCtrl->SetValue(newText);
    }
}

void CanvasTextElement::OnTextEnter() {
    StopEditing();
}

void CanvasTextElement::OnTextKillFocus() {
    StopEditing();
}

wxRect CanvasTextElement::GetBounds() const{
    return wxRect(m_position, m_size);
}

wxPoint CanvasTextElement::GetPosition() const {
	return m_position;
}
