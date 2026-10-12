# 给 DeepSeek 的 GUI 升级交接

更新：2026-10-12。项目是 C++20 / wxWidgets 3.2.9 的 SigFlow FPGA 桌面应用，工作目录为 `E:\EDA_Race\Cangku\new\SigFlow_FPGA_Cmake`。本轮目标是把 `/GUI` 的 OpenDesign 视觉与交互落到现有程序，同时保留工程、网表、综合和仿真业务。

## 先看什么

- **2026-10-12 新一轮圆角/统一风格规划**：用户新增的区域圆角、卡片层级、页签/图标/表单统一需求见 [GUI-ROUND2](GUI-ROUND2/README.md)。当前已推进 RV-T00/RV-T02/RV-T03：新增 `UiRegionHost` 并接入 `sideNav`、`left_sidebar`、`center_area`、`right_sidebar`、`bottom_tabs`，产品已编译通过；真实窗口视觉和布局回归尚未完成，不能把这批代码当成签收。

- `GUI/app.html` 是主要视觉输入，`GUI/index.html` 补充细节；`GUI/` 保持冻结。
- [GUI 工程 SPEC](GUI-CHUGAO/SigFlow-界面改造SPEC.md)、[任务清单](GUI-CHUGAO/SigFlow-GUI-Implementation-Task.md)、[追溯矩阵](GUI-CHUGAO/SigFlow-GUI-Traceability-Matrix.md) 已工程化拆分。**当前进度以 [acceptance.md](GUI-CHUGAO/acceptance.md) 最新章节为准**，早期任务勾选和截图是历史快照。
- `docs/gui/task.md`、`docs/agent/task.md` 是此前两套方案的执行清单；与 OpenDesign 冲突时按 SPEC 的 DEC 裁决及用户最新要求处理。
- 不熟悉测试夹具时，先看 [夹具使用说明](GUI-CHUGAO/夹具使用说明.md)：入口替换、构建运行顺序、日志位置和失败判断都在里面，照着跑即可。

## 已经做了什么

| 范围 | 实现与关键位置 |
| --- | --- |
| 统一视觉基础 | `main/ui/UiTheme.*` 集中颜色、字体、间距、DIP 与动效；新增 `RegionMetrics` 统一宿主内缩、字段高度/圆角和卡片间距；Dock/Tab/Toolbar Art 和现有面板接入。终端单独保留深底可读配色。 |
| 主窗与侧栏 | 六区骨架、菜单/工具栏、状态栏、树/元件库、Property/Pin Binding/DeepSeek 表单及波形样式；输入高度至少 28 DIP，原生输入 focus 定制仍受平台限制。 |
| T-410 画布 | 选中标记、幽灵、平移、放置、分栏；按用户要求，元件**透明且没有容器边框/阴影**。修复引脚连线点击/拖放、捕获与取消、缩放命中、PNG 光标 invalid image 断言。 |
| T-420 工具与 HUD | `CanvasControls.h` 与 `CanvasPanel`：Select/Text/Component/Wire/Pan/Eraser，25–400% 缩放，滚轮锚点，Fit 回到 100% 与原点；与现有工具状态机同步。 |
| T-430 编辑器 | `EditorChrome.h`、`SigTextEditor`：34 DIP 文件头、语言/校验状态、13 DIP 等宽与行距、语法色及 gutter；编辑后复位等待状态，解析器/VerilogManager 不改。 |
| T-510 布局 | `UiLayoutState.*`、`UiLayoutPicker.*`、`MainFrame`：六栏显隐、四预设、主侧栏左右、面板顶部/底部与四种对齐、保存/恢复、Quick Pick、布局快捷键与子窗焦点隔离。 |
| 玻璃与按钮反馈 | `UiToolBarArt.*` 改为柔化投影、渐变玻璃与方向性高光，移除多重硬环；工具栏与布局按钮蓝边只在 Hover/按压出现，动作反馈最长 140 ms，焦点保留改用灰色提示。见 DEC-14 与验收 §22/26。 |
| ROUND2 容器/页签 | `UiRegionHost.*` 包裹五个真实区域；AUI/Notebook/Tree 内部方框已去重；`UiTabArt` 平面圆角自绘、激活 accent 指示线和无默认背景横线；SidePanel DeepSeek 页由宿主统一底色与无框输入。 |
| ROUND2 列表/表单/终端 | 三棵树优先加载项目单色 SVG 并按 DIP 回退；`PropertyPanelBuilder` 的原生 TextCtrl/Choice 外加 `UiFieldChrome` 圆角壳，保留 ID/事件；FPGA Pin Binding 输入高度统一；终端 selection/caret/空白区角色显式绑定 `TerminalPalette`。 |
| T-520 / T-620 | DeepSeek 补了 1024/760 DIP 断点、最小窗、dock clamp、内嵌控件隐藏与等价入口、工具栏方向键/Space/Enter、显式 Tab 链。完整人工 Tab 走查仍待做。 |
| T-640 / T-660 | 已加 15 组浅色对比度检查、DPI/显示器变化重算、字体重设与布局性能闸门。2026-10-12 修正离屏恢复：移动实际外层 `pane.frame`，保留子窗坐标。 |

## 已确认的边界

保留原生标题栏/菜单、既有命令与快捷键；右栏本轮保留，IA 迁移 T-600 后置。Synthesize 追加入口沿用 `DoFpgaSynthesis()` 打开原工具，不直接启动综合任务。独立窗口 T-500 移出本轮；深色继续按 DEC-11 暂缓。

**DEC-14 包括键盘聚焦：蓝边只在 Hover / 按压出现，idle 焦点用灰线。** DeepSeek 曾以 DEC-15 把键盘蓝环加回；这是实施者解释，没有新的用户授权，已撤回。其它控件的可见焦点提示保留。

业务保护范围见 SPEC SAFE：不顺手改模型、网表、解析、插件或 EDA 路径。T-410 连线事件的局部修复由用户明确授权，视觉调整不扩大该例外。工作区有大量连续批次未提交成果；保留它们，禁止 stash/reset/clean，保持原文件 BOM/EOL。

## 构建、夹具与下一步

可运行程序：`build-local/sigflow.exe`。本轮最后一次 Windows 构建成功（`[100%] Built target sigflow`），产物 55,562,848 B，SHA-256 `3EE8D7F9BAD4DB5A1A94FFBF9CEC633D7679E74E1870FBF9C15B2FF011088B7B`；视觉截图、窄屏、浮动/分裂、IME/Tab 和真实画布连线仍需在 Windows 前台手工验收，Linux 按用户决定不执行。

```powershell
& 'D:/Program_Files/CMake/bin/cmake.exe' --build build-local --target sigflow -j 4
```

夹具是替换 `cMain.cpp` 入口、复用正式产品对象的独立测试 exe，不是正式启动参数。先构建产品，再构建夹具，再运行；收到 `session_id` 就继续等到退出，链接中的 exe 可能为 0 B。配置与样例在 `build-local` 隔离，不能复制 `DeleteAll()` 到产品入口。使用指南见上方《夹具使用说明》；本轮新增 [T700 验证记录](GUI-CHUGAO/_evidence/T700-verification.md)，覆盖键盘蓝边、离屏恢复、编辑内容保留、命令路由与 20 次布局切换。源码审计依赖动手前快照，不能用当前源码重建旧基线。

同一目标的构建串行；不同夹具链接可并行，**GUI 运行必须串行**。原生键盘回归须交互桌面，约 20 秒保持测试窗前台；以完整日志、RESULT、进程 exit 0 共同判定，失焦或前置条件失败不能算通过，也不能直接认定是产品缺陷。

本轮 Windows 合同20、逻辑225、集成136、布局151、窄屏93、编辑器21、源码边界51通过。画布有452/0成功，但末次复跑452/33，缺前台校验，稳定性/归因待核；不要把测试失败直接等同产品崩溃。

**用户最新范围：先不在 Linux 验证，本轮按 Windows 收口。** 已执行的 Linux 构建通过记录保留，不再追加 Linux 验证，也不作为当前签收条件。

ROUND2 的实现任务已全部推进到代码完成/待验收状态（详见 [GUI-ROUND2/task.md](GUI-ROUND2/task.md) 与 [acceptance.md](GUI-ROUND2/acceptance.md)）；剩余工作是按 validation.md 逐项在 Windows 实际窗口复核并记录截图/交互日志，不能用编译通过替代视觉签收。
