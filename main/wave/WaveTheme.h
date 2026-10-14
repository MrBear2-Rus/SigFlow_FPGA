#pragma once

// T-325 / THE-034：WaveTheme 保留**既有语义与取值**（信号/轨道语义色），
// 只新增一层适配，把背景 / 网格 / 文字接到唯一 Token 层 UiTheme（SAFE-012）。

#include "ui/UiTheme.h"

#include <wx/colour.h>

#include <cstddef>
#include <string>

namespace sigflow {
namespace wave {

enum class WaveTheme {
    Dark,
    Light
};

struct WaveThemeColors {
    wxColour background;
    wxColour grid;
    wxColour text;
    wxColour miniMapBackground;
    wxColour viewport;
};

inline WaveThemeColors ColorsFor(WaveTheme theme)
{
    if (theme == WaveTheme::Light) {
        return { wxColour(250, 250, 249), wxColour(214, 211, 209),
                 wxColour(28, 25, 23), wxColour(231, 229, 228),
                 wxColour(37, 99, 235, 48) };
    }
    return { wxColour(30, 30, 32), wxColour(45, 45, 50),
             wxColour(203, 213, 225), wxColour(24, 24, 27),
             wxColour(59, 130, 246, 45) };
}

// ═══════════════════════════════════════════════════════════════════════════
// T-325 适配层（THE-034 / CMP-064）
//
// 旧表 ColorsFor(WaveTheme) 表达的是"波形自带深/浅观察档"，取值予以保留；
// 本层在此之上把**背景 / 网格 / 文字 / 缩略图底 / 视口高亮**改接 UiTheme
// token（SPEC §3.3），语义信号色按 CMP-064 取自 token 语义族。
// 消费点用 ColorsForThemed()，GL 与软件两条后端共用同一入口，保证一致。
// ═══════════════════════════════════════════════════════════════════════════

/// 波形观察档 → UiTheme 档位（波形自带 Light/Dark 切换，与 token 档位一一对应）
inline UiTheme::Mode UiModeFor(WaveTheme theme)
{
    return theme == WaveTheme::Light ? UiTheme::Mode::Light : UiTheme::Mode::Dark;
}

/// 按 alpha 把 fg 预合成于 bg：wx 的 GDI 绘制路径丢弃 alpha，
/// 自绘/画刷必须给实色（SPEC §3.5 / THE-021），故此处给出计算值而非字面量。
inline wxColour CompositeOver(const wxColour& fg, const wxColour& bg, double alpha)
{
    const double a = alpha < 0.0 ? 0.0 : (alpha > 1.0 ? 1.0 : alpha);
    const auto mix = [a](unsigned char front, unsigned char back) {
        return static_cast<unsigned char>(a * static_cast<double>(front) +
                                          (1.0 - a) * static_cast<double>(back) + 0.5);
    };
    return wxColour(mix(fg.Red(), bg.Red()), mix(fg.Green(), bg.Green()),
                    mix(fg.Blue(), bg.Blue()));
}

/// token 化档位表：背景/网格/文字/缩略图底/视口取 UiTheme，旧字段语义不变
inline WaveThemeColors ColorsFor(WaveTheme theme, const UiTheme::Colours& ui)
{
    WaveThemeColors colors = ColorsFor(theme);   // 旧取值先落位（THE-034）
    colors.background        = ui.canvas;        // 波形底       = canvas
    colors.grid              = ui.grid;          // 细网格       = grid
    colors.text              = ui.text2;         // 次级文字     = text2
    colors.miniMapBackground = ui.bgDeep;        // 缩略图底     = bgDeep
    colors.viewport          = ui.selOnPanel;    // 视口高亮     = sel 合成实色
    return colors;
}

/// 便捷入口：按波形观察档取对应 token 档（消费点统一走这里）
inline WaveThemeColors ColorsForThemed(WaveTheme theme)
{
    return ColorsFor(theme, UiTheme::PaletteFor(UiModeFor(theme)));
}

/// CMP-064：四类信号语义（clk=brand / rst=accent / 总线=warn / 其它=text1 82%）
enum class WaveSignalRole {
    Clock,
    Reset,
    Bus,
    Data
};

namespace detail {

inline bool IsNameToken(const std::string& segment, const char* const* tokens,
                        std::size_t count)
{
    for (std::size_t i = 0; i < count; ++i) {
        if (segment == tokens[i]) return true;
    }
    return false;
}

/// 名称按"非字母数字"切段后整段比对：rst_n / n_rst / arst_n 可命中，
/// 而 burst / first 这类子串不会被误判为复位。
inline bool HasNameToken(const std::string& lower, const char* const* tokens,
                         std::size_t count)
{
    std::string segment;
    for (const char c : lower) {
        const bool alnum = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
        if (alnum) {
            segment.push_back(c);
            continue;
        }
        if (!segment.empty() && IsNameToken(segment, tokens, count)) return true;
        segment.clear();
    }
    return !segment.empty() && IsNameToken(segment, tokens, count);
}

} // namespace detail

/// 信号名 + 是否总线 → 语义类（名称优先于宽度；向量信号视作总线）
inline WaveSignalRole WaveSignalRoleOf(const std::string& name, bool isBus)
{
    static const char* const kClockTokens[] = { "clk", "clock", "clki", "clko" };
    static const char* const kResetTokens[] = { "rst", "reset", "arst", "nrst",
                                                "rstn", "resetn" };
    std::string lower = name;
    for (char& c : lower) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    if (detail::HasNameToken(lower, kClockTokens,
                             sizeof(kClockTokens) / sizeof(kClockTokens[0]))) {
        return WaveSignalRole::Clock;
    }
    if (detail::HasNameToken(lower, kResetTokens,
                             sizeof(kResetTokens) / sizeof(kResetTokens[0]))) {
        return WaveSignalRole::Reset;
    }
    return isBus ? WaveSignalRole::Bus : WaveSignalRole::Data;
}

/// 语义类 → 最终取值（CMP-064）
inline wxColour SignalColour(WaveSignalRole role, const UiTheme::Colours& ui)
{
    switch (role) {
    case WaveSignalRole::Clock: return ui.brand;
    case WaveSignalRole::Reset: return ui.accent;
    case WaveSignalRole::Bus:   return ui.warn;
    case WaveSignalRole::Data:  break;
    }
    // 其它信号：text1 @82% 预合成于 canvas（GDI 无 alpha，给计算实色）
    return CompositeOver(ui.text1, ui.canvas, 0.82);
}

inline wxColour SignalColour(const std::string& name, bool isBus,
                             const UiTheme::Colours& ui)
{
    return SignalColour(WaveSignalRoleOf(name, isBus), ui);
}

inline const char* ThemeName(WaveTheme theme)
{
    return theme == WaveTheme::Light ? "light" : "dark";
}

inline WaveTheme ThemeFromName(const std::string& name)
{
    return name == "light" ? WaveTheme::Light : WaveTheme::Dark;
}

} // namespace wave
} // namespace sigflow
