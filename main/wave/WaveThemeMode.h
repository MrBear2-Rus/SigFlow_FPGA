#pragma once

#include <string>

namespace sigflow::wave {

// The persisted observation mode is independent of GUI colours and wxWidgets.
enum class WaveTheme { Dark, Light };

inline const char* ThemeName(WaveTheme theme)
{
    return theme == WaveTheme::Light ? "light" : "dark";
}

inline WaveTheme ThemeFromName(const std::string& name)
{
    return name == "light" ? WaveTheme::Light : WaveTheme::Dark;
}

} // namespace sigflow::wave
