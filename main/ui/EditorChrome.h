#pragma once

#include "ui/UiTheme.h"
#include <wx/control.h>
#include <wx/dcbuffer.h>
#include <wx/filename.h>
#include <wx/graphics.h>
#include <wx/panel.h>
#include <wx/sizer.h>
#include <algorithm>
#include <memory>

// T-430: chrome around the existing wxStyledTextCtrl.  It owns presentation
// only; parsing, saving and diagnostics stay in SigTextEditor/VerilogManager.
class EditorHeaderBar final : public wxControl {
public:
    explicit EditorHeaderBar(wxWindow* parent)
        : wxControl(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE) {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetMinSize(FromDIP(wxSize(-1, 34)));
        m_themeSubscription = UiTheme::Subscribe([this] { Refresh(false); });
        Bind(wxEVT_PAINT, [this](wxPaintEvent&) { Paint(); });
    }

    void SetDocument(const wxString& path, const wxString& language, bool validated) {
        m_title = path.IsEmpty() ? "untitled.v" : wxFileName(path).GetFullName();
        m_language = language;
        m_validated = validated;
        Refresh(false);
    }

    wxString Title() const { return m_title; }
    wxString Language() const { return m_language; }
    bool IsValidated() const { return m_validated; }

private:
    void Paint() {
        wxAutoBufferedPaintDC dc(this);
        const auto& pal = UiTheme::Palette();
        dc.SetBackground(wxBrush(pal.surfaceOnCanvas));
        dc.Clear();
        std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
        if (!gc) return;

        const double dip = FromDIP(100) / 100.0;
        const auto rect = GetClientRect();
        gc->SetPen(gc->CreatePen(wxGraphicsPenInfo(pal.strokeOnPanel).Width(dip)));
        gc->StrokeLine(0, rect.height - dip / 2.0, rect.width, rect.height - dip / 2.0);

        // 15 DIP outline document icon from GUI/app.html.
        gc->SetBrush(*wxTRANSPARENT_BRUSH);
        gc->SetPen(gc->CreatePen(wxGraphicsPenInfo(pal.text3).Width(1.7 * dip)));
        const double x = FromDIP(12), y = (rect.height - FromDIP(15)) / 2.0;
        auto icon = gc->CreatePath();
        icon.MoveToPoint(x + FromDIP(2), y);
        icon.AddLineToPoint(x + FromDIP(9), y);
        icon.AddLineToPoint(x + FromDIP(13), y + FromDIP(4));
        icon.AddLineToPoint(x + FromDIP(13), y + FromDIP(15));
        icon.AddLineToPoint(x + FromDIP(2), y + FromDIP(15));
        icon.CloseSubpath();
        icon.MoveToPoint(x + FromDIP(9), y);
        icon.AddLineToPoint(x + FromDIP(9), y + FromDIP(4));
        icon.AddLineToPoint(x + FromDIP(13), y + FromDIP(4));
        gc->StrokePath(icon);

        gc->SetFont(UiTheme::UiFont(this, UiTheme::TextSize::P14), pal.text1);
        const double textX = x + FromDIP(22);
        const double textY = (rect.height - FromDIP(14)) / 2.0;
        gc->DrawText(m_title, textX, textY);
        double titleW = 0, titleH = 0;
        gc->GetTextExtent(m_title, &titleW, &titleH);
        gc->SetFont(UiTheme::UiFont(this, UiTheme::TextSize::P11), pal.text3);
        gc->DrawText(wxString::FromUTF8("— ") + m_language, textX + titleW + FromDIP(7),
                     (rect.height - FromDIP(11)) / 2.0);

        const wxString badge = m_validated
            ? wxString::FromUTF8("Tree-sitter · 已校验")
            : wxString::FromUTF8("Tree-sitter · 等待校验");
        double badgeW = 0, badgeH = 0;
        gc->GetTextExtent(badge, &badgeW, &badgeH);
        gc->DrawText(badge, std::max<double>(textX, rect.width - badgeW - FromDIP(12)),
                     (rect.height - badgeH) / 2.0);
    }

    wxString m_title = "untitled.v";
    wxString m_language = "Verilog";
    bool m_validated = false;
    UiTheme::Subscription m_themeSubscription;
};

class EditorChrome final : public wxPanel {
public:
    explicit EditorChrome(wxWindow* parent)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE) {
        SetBackgroundColour(UiTheme::Palette().surfaceOnCanvas);
        m_header = new EditorHeaderBar(this);
        m_sizer = new wxBoxSizer(wxVERTICAL);
        m_sizer->Add(m_header, 0, wxEXPAND);
        SetSizer(m_sizer);
    }

    void SetEditor(wxWindow* editor) {
        m_sizer->Add(editor, 1, wxEXPAND);
        Layout();
    }

    void SetDocument(const wxString& path, const wxString& language, bool validated) {
        m_header->SetDocument(path, language, validated);
    }

    wxString Title() const { return m_header->Title(); }
    wxString Language() const { return m_header->Language(); }
    bool IsValidated() const { return m_header->IsValidated(); }

private:
    EditorHeaderBar* m_header = nullptr;
    wxBoxSizer* m_sizer = nullptr;
};
