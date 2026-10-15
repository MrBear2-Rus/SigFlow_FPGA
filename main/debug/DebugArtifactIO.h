#pragma once
#include <filesystem>
#include <fstream>
#include <string>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace sigflow::debug {
inline bool WriteTextFileAtomic(const std::filesystem::path& path, const std::string& content,
                                std::string& error)
{
    error.clear();
    std::error_code ec;
    if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) { error = "unable to create artifact directory: " + ec.message(); return false; }
    auto temp = path;
    temp += ".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) { error = "unable to create temporary artifact: " + temp.string(); return false; }
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
        out.flush();
        if (!out) { error = "unable to write temporary artifact: " + temp.string(); return false; }
    }
#if defined(_WIN32)
    if (!MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        ec = std::error_code(GetLastError(), std::system_category());
#else
    std::filesystem::rename(temp, path, ec);
#endif
    if (ec) {
        error = "unable to commit artifact: " + path.string() + ": " + ec.message();
        std::error_code ignored;
        std::filesystem::remove(temp, ignored);
        return false;
    }
    return true;
}
} // namespace sigflow::debug
