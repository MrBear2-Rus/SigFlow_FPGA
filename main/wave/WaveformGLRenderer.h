#pragma once

#include "WaveformRenderData.h"

#include <cstdint>
#include <vector>

namespace sigflow {
namespace wave {

// OpenGL 批量渲染器（T-W2-01/02）：
// - 固定管线 + VBO 批量提交线段（兼容性最好，无需 shader 编译）；
// - 文字层以 RGBA 纹理叠加，解决 wxDC + SwapBuffers 闪烁。
class WaveformGLRenderer {
public:
    WaveformGLRenderer() = default;
    ~WaveformGLRenderer();

    // 需要在当前 GL 上下文有效时调用。
    bool Initialize();
    void Shutdown();
    bool IsInitialized() const { return m_initialized; }

    void BeginFrame(int width, int height, const WaveColor& background);
    void EndFrame();

    void DrawPolyline(const std::vector<WaveformPoint>& points,
                      const WaveColor& color);
    void DrawGridLine(float x0, float y0, float x1, float y1,
                      const WaveColor& color);

    void UploadTextOverlay(int width, int height, const std::uint8_t* rgba);
    void DrawTextOverlay();

    int FrameWidth() const { return m_frameWidth; }
    int FrameHeight() const { return m_frameHeight; }

private:
    void SetupOrtho(int width, int height);

    bool m_initialized = false;
    unsigned int m_texture = 0;
    int m_textureWidth = 0;
    int m_textureHeight = 0;
    int m_frameWidth = 0;
    int m_frameHeight = 0;
};

} // namespace wave
} // namespace sigflow
