#pragma once

#include "platform/PlatformPaths.h"
#include <wx/cursor.h>
#include <wx/image.h>
#include <wx/imagpng.h>
#include <wx/log.h>

namespace UiImageResources {
// wxApp 只注册基础图像处理器；PNG 必须在创建任何窗口前显式注册。
// 检查类型后再注册，允许入口和测试重复调用，不产生重复 handler 警告。
inline void EnsurePngHandler()
{
    if (!wxImage::FindHandler(wxBITMAP_TYPE_PNG))
        wxImage::AddHandler(new wxPNGHandler);
}

inline wxCursor LoadImageCursor(const wxString& path, wxStockCursor fallback)
{
    if (!wxImage::FindHandler(wxBITMAP_TYPE_PNG) || !wxFileExists(path))
        return wxCursor(fallback);
    wxImage image;
    // 可选光标资源失败时使用系统光标，避免反复弹出文件/解码错误。
    wxLogNull quiet;
    if (!image.LoadFile(path, wxBITMAP_TYPE_PNG) || !image.IsOk())
        return wxCursor(fallback);
    wxCursor cursor(image);
    return cursor.IsOk() ? cursor : wxCursor(fallback);
}
} // namespace UiImageResources
