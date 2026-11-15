# Edu Agent 内容浏览验证记录

基线为 `f7fe3cb`。验证日期为 2026-11-15。环境为 Windows、MinGW CMake Debug 和随库 wxWidgets 3.3。测试对象是原生工作台及其子控件。正式宿主链路使用本机 Core、Yosys 和 Verilator。

## 实施范围

- 工作台响应折叠事件，重新计算教学辅助的内容高度和原生滚动范围。侧栏定位按目标矩形计算内外层视口。窗口重排和形态切换复用阅读位置，教学辅助以分区及分区内偏移为锚点。
- 竖向滚轮先交给指针下可沿该方向滚动的最近区域，再交给整页。正文支持翻页键，输入框继续处理编辑按键。保留水平滚动、文本选择及原生滑块。
- 滚动条占用宽度变化后重新测量正文。学习路径的固定尾注在短窗中转入滚动内容，长说明按字体及控件内边距计算高度。
- 课程引用、学习记录等辅助正文可直接选择并用 Ctrl+C 复制，复用现有区域。限量说明由原有投影展示。更早时间线分页按 READ-07 单独处理。

## 回归结果

| 范围 | 结果 | 验证方式 |
| --- | --- | --- |
| 原始缺陷 | 基线失败 | 在 `f7fe3cb` 的工作台和对话控件运行同一组核心回归。展开不更新滚动范围、滚动底端不可达、正文上的滚轮无效均触发断言。 |
| 工作台原生控件 | 通过 | Windows 原生控件在 144 DPI（150%）验证折叠、长正文末行、滚动区交接、侧栏重复定位、阅读锚点、键盘和 375 DIP 短窗口。滑块路径覆盖 Windows 滚动消息和 wxWidgets 拖动事件。 |
| 正式宿主 | 通过 | `u2-native-reading-acceptance.json` 记录 3 个真实 Job。教学时间线、学习记录、课程上下文、复测检查均验证通过。模型调用数为 0。 |
| 主窗口人工签收 | 待验证 | 主窗口和浮窗上的实体鼠标拖动、停靠切换、100%/125% 系统缩放，以及从主窗口打开全加器后走完整教学操作。 |
| 较早记录分页 | 待接口确认 | READ-07 涉及正式宿主契约。 |

## 复测

从仓库根目录执行。

```powershell
cmake -S . -B build-main-msvcrt -DSIGFLOW_BUILD_AGENT_GUI_TESTS=ON
cmake --build build-main-msvcrt --target edu_native_reading_smoke edu_u2_gateway --parallel 6
ctest --test-dir build-main-msvcrt -R '^edu_native_reading_smoke$' --output-on-failure -V
```

主界面手工签收使用 `build-main-msvcrt/sigflow.exe` 打开 `examples/full_adder`。运行仿真后从 Agent 工作台选取当前报告，开始规则教学并完成提示与结束操作。展开课程引用、证据链和学习记录，分别把鼠标放在正文和边缘滚动，再拖动滑块至两端。调整为短窄浮窗，复查学习路径、输入区和已展开分区。
