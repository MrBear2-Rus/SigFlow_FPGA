# CanvasNoteBook 双缓冲绘制断言修复

日期：2026-10-23。基线：f6c0b00。

## 原因与修正

CanvasNoteBook 构造时注册 OnPaintEmpty，该处理器使用 wxAutoBufferedPaintDC 绘制完整背景，但构造函数没有设置 wxBG_STYLE_PAINT。当前 wxWidgets 的 dcbuffer.h:225 对这个前置条件执行调试断言，首次显示画布、打开工程或窗口重绘均可能触发弹窗。

在 CanvasNoteBook 构造函数中、注册绘制事件之前设置 SetBackgroundStyle(wxBG_STYLE_PAINT)。保留既有主题背景与网格绘制，也保留 Debug 断言。父窗口或 CanvasPanel 设置相同样式不能替代笔记本窗口自身的设置。

代码核查显示 main 下其他直接使用 wxAutoBufferedPaintDC 的控件均已有对应背景样式。本次只修改 CanvasNoteBook 的构造设置，不涉及工程文件、Agent 服务、模型配置或系统环境。

## 验证与复查

构建入口：

```powershell
cmake --build build-main-msvcrt --target sigflow -j 3
```

本轮按操作要求不自动启动主程序。构建结果记录于工作区 `.superpowers/sdd/wx-paint-style-fix-20261023`；人工窗口重绘尚未执行，不能将编译结果视为完整 GUI 验收。

手动复查步骤：

1. 关闭旧 SigFlow 进程，启动本轮生成的 build-main-msvcrt/sigflow.exe。
2. 显示空画布，打开 examples/tracebridge_tangnano9k 工程。
3. 切换源码/画布页、调整窗口大小、关闭并重新打开源码页。
4. 确认背景和网格正常，未出现同一 wxAutoBufferedPaintDC 背景样式断言。

如果仍出现弹窗，应核对实际启动的可执行文件路径和修改时间，并展开详细信息采集新的调用栈。不能通过禁用断言或勾选不再显示来判断修复完成。
