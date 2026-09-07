#pragma once

#include <wx/colour.h>

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
