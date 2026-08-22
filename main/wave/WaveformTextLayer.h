#pragma once

#include "WaveViewState.h"
#include "WaveformRenderData.h"

#include <wx/bitmap.h>
#include <wx/window.h>

namespace sigflow {
namespace wave {

// 生成文字层位图（信号名 / 时间轴刻度 / 向量值标注）。
// 软件后端直接 blit；GL 后端转 RGBA 后上传纹理。
bool BuildTextLayerBitmap(wxWindow* win, const WaveformFrame& frame,
                          const WaveViewState& state, int width, int height,
                          wxBitmap& outBitmap);

} // namespace wave
} // namespace sigflow
