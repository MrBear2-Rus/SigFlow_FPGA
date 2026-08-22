#include "WaveformGLRenderer.h"

#ifdef _DEBUG
#pragma comment(lib, "wxmsw32ud_gl.lib")
#else
#pragma comment(lib, "wxmsw32u_gl.lib")
#endif
#pragma comment(lib, "opengl32.lib")

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <GL/gl.h>

// 说明：opengl32.dll 只导出 GL 1.1 符号，VBO（1.5）需 wglGetProcAddress 加载。
// 当前使用 GL 1.1 立即模式绘制线段，保证兼容性；后续可按需切换 VBO 批量。

namespace sigflow {
namespace wave {

namespace {

void Color4f(const WaveColor& color, float alpha = 1.0f)
{
    glColor4f(color.r, color.g, color.b, alpha);
}

} // namespace

WaveformGLRenderer::~WaveformGLRenderer()
{
    Shutdown();
}

bool WaveformGLRenderer::Initialize()
{
    if (m_initialized) return true;

    glGenTextures(1, &m_texture);
    m_initialized = (m_texture != 0);
    return m_initialized;
}

void WaveformGLRenderer::Shutdown()
{
    if (!m_initialized) return;
    if (m_texture) {
        glDeleteTextures(1, &m_texture);
        m_texture = 0;
    }
    m_initialized = false;
}

void WaveformGLRenderer::SetupOrtho(int width, int height)
{
    glViewport(0, 0, width, height);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0.0, static_cast<double>(width),
            static_cast<double>(height), 0.0, -1.0, 1.0);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
}

void WaveformGLRenderer::BeginFrame(int width, int height)
{
    m_frameWidth = width;
    m_frameHeight = height;
    SetupOrtho(width, height);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_TEXTURE_2D);
    glClearColor(30.0f / 255.0f, 30.0f / 255.0f, 32.0f / 255.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
}

void WaveformGLRenderer::EndFrame()
{
    glFlush();
}

void WaveformGLRenderer::DrawPolyline(const std::vector<WaveformPoint>& points,
                                      const WaveColor& color)
{
    if (points.size() < 2) return;
    Color4f(color);
    glBegin(GL_LINES);
    for (std::size_t i = 0; i + 1 < points.size(); ++i) {
        glVertex2f(points[i].x, points[i].y);
        glVertex2f(points[i + 1].x, points[i + 1].y);
    }
    glEnd();
}

void WaveformGLRenderer::DrawGridLine(float x0, float y0, float x1, float y1,
                                      const WaveColor& color)
{
    Color4f(color);
    glBegin(GL_LINES);
    glVertex2f(x0, y0);
    glVertex2f(x1, y1);
    glEnd();
}

void WaveformGLRenderer::UploadTextOverlay(int width, int height,
                                           const std::uint8_t* rgba)
{
    if (!m_texture) return;
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, m_texture);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    m_textureWidth = width;
    m_textureHeight = height;
}

void WaveformGLRenderer::DrawTextOverlay()
{
    if (!m_texture || m_textureWidth <= 0 || m_textureHeight <= 0) return;
    glEnable(GL_TEXTURE_2D);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
    glBindTexture(GL_TEXTURE_2D, m_texture);

    const int w = m_textureWidth;
    const int h = m_textureHeight;
    glBegin(GL_QUADS);
    glTexCoord2f(0.0f, 0.0f); glVertex2i(0, 0);
    glTexCoord2f(1.0f, 0.0f); glVertex2i(w, 0);
    glTexCoord2f(1.0f, 1.0f); glVertex2i(w, h);
    glTexCoord2f(0.0f, 1.0f); glVertex2i(0, h);
    glEnd();

    glDisable(GL_BLEND);
    glDisable(GL_TEXTURE_2D);
}

} // namespace wave
} // namespace sigflow
