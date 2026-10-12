# 第二轮实施与验收记录

> 2026-10-12 · **容器接入代码已实施，视觉验收仍待实际窗口回归**。本记录同时保留规划边界与当前产物证据。

## 1. 起点

- 继承第一轮 [acceptance §28](../GUI-CHUGAO/acceptance.md)：深色/子窗口/字体/键盘等已有实现，不再以 §26 的旧缺口描述为当前状态。
- 文档编写时只读核对的 Windows exe：`build-local/sigflow.exe`，55,483,043 B，2026-10-12 17:52:38，SHA-256 `5E2857F82D29BF43F972AFCFC2C016EFC2E25FB9C13EDAA05CA8515E345CBA55`。这不是本轮新编译结果，也不能证明用户现状截图就是该版本。
- 用户两张原图已保留在 `_reference/`，记录见 manifest。未裁剪/缩放/编辑；附件 DPI 和实际程序版本未确认。
- 工作区已有大量连续批次未提交改动；本次没有清理、提交或回退它们。
- Linux 按用户指示不验证。

## 2. 本批实施记录（RV-T00～RV-T08 代码批次）

- **日期**：2026-10-12；**负责人**：Codex。
- **RV-T00 只读核对**：保留现有大量未提交改动；本批最终 `build-local/sigflow.exe` 为 55,562,848 B，mtime `2026-10-12 23:53:39`，SHA-256 `3EE8D7F9BAD4DB5A1A94FFBF9CEC633D7679E74E1870FBF9C15B2FF011088B7B`。没有清理、提交、回退或修改用户配置。
- **RV-T02 / RV-T03 实施**：新增 `main/ui/UiRegionHost.h/.cpp`，用 `wxGraphicsContext` 绘制 `bgPanel` 圆角实底和 `strokeOnPanel` 描边；内容默认内缩 8 DIP，保留原 content `wxWindow*`、`UiTheme::Balance` 对齐和主题订阅。`MainFrame` 的 `sideNav`、`left_sidebar`、`center_area`、`right_sidebar`、`bottom_tabs` 均已改为 `UiRegionHost → 原 content`；`UiTheme::ApplyTheme` 已增加 host 内容解包，继续刷新原有 TabArt、ToolBarArt 和页面树。原 `wxAuiNotebook`、`wxSimplebook`、splitter、Terminal/Waveform 页面和布局状态接口未替换。
- **边框去重修正**：`UiDockArt` 的 AUI pane border 设为 0；画布、右侧和底部 `wxAuiNotebook` 以及左侧 `wxSimplebook` 显式使用 `wxBORDER_NONE`，移除圆角 host 内部重复的方形容器框。该修正只触达 UI 外观，不改变 pane 名称、页面句柄或业务路径。
- **平面嵌入修正**：工程树、逻辑树、组件库树显式关闭原生树边框；`UiTabArt::DrawBorder` 改为绘制无边框平面底色，避免页签容器再绘制一层直角外框。真实窗口截图仍待用户复核。
- **RV-T04 增量**：`UiTabArt` 继续复用 AUI 的文字、位图、关闭按钮和命中矩形计算，新增零额外边框空间，并在激活页叠加主题 `accent` 细指示线；未改页签关闭、拖动、分裂或键盘路由。
- **RV-T04 自绘增量**：页签绘制改为平面圆角标签，保留 AUI 的尺寸测量、关闭按钮和命中矩形；普通页签使用 `surfaceOnPanel`，激活页使用 `strongOnPanel` 与 `accent` 指示线，hover 保留轻微底色反馈。真实窗口截图与窄栏溢出仍待复核。
- **SidePanel 宿主适配**：仅在宿主侧对 DeepSeek Assistant 页的原生列表/文本容器去除默认方框，并将面板底、列表底、历史区和输入区映射到当前主题；插件会话、网络、按钮事件和数据路径未修改。
- **SidePanel 页签背景修正**：`UiTabArt::DrawBackground` 不再调用 wxAUI 默认背景绘制，移除页签下方残留的直角横线；页签形状和激活指示线由本轮自绘路径独立负责。
- **RV-T01 token/几何增量**：`UiTheme::RegionMetrics` 集中内容内缩、输入最小高/圆角、字段内距、卡片间距和阴影预留；`UiRegionHost` 与表单 builder 共享这些 DIP 预算。未改写旧 palette、主题持久化或业务状态。
- **RV-T05 图标增量**：工程树、逻辑树、组件库树去除重复原生边框；项目/模块 SVG 优先，资源缺失时回退有效 bitmap；组件库图标列表按当前窗口 DIP 尺度建立。
- **RV-T06 表单增量**：`PropertyPanelBuilder` 增加 `UiFieldChrome` 圆角输入壳，原生 `wxTextCtrl`/`wxChoice` 实例和 ID/事件仍由原代码持有；FPGA Pin Binding 的列表、字段和按钮同步去掉内框并使用 28 DIP 最小高度。
- **RV-T07 终端增量**：终端沿用既有六类 style ID 和 `TerminalPalette`，补齐选择背景/前景、光标、空白区语义色；不触碰输出文本与命令控制器。
- **保护范围**：未改画布连线、树模型、属性读写、网表、解析、综合、仿真或插件业务路径；尚未改变 pane 名称、布局字段或业务窗口句柄。

### 2.1 构建结果

```text
command: D:/Program_Files/CMake/bin/cmake.exe --build build-local --target sigflow -j 4
result: [100%] Built target sigflow
exit: 0
```

首次编译发现并修复了新文件的 `<memory>` 缺失和 graphics rectangle 的整数/浮点类型错误；修复后重编译通过。配置阶段曾报告 Windows 创建 wx-config symbolic link 需要权限，但 CMake 随后正常生成且目标成功完成；该提示不代表产品编译失败。

### 2.2 当前判定

| 项 | 状态 | 说明 |
| --- | --- | --- |
| RV-T00 | `[~]` | 代码/产物基线已记录；同工程截图、布局句柄清单和性能基线仍待夹具执行 |
| RV-T01 | `[~]` | RegionMetrics 与终端选择/空白区角色已落地；独立对比度/运行时验证待执行 |
| RV-T02 | `[~]` | 共享容器代码已通过编译；尚未运行真实窗口四角、滚动、页签、焦点和命中验证 |
| RV-T03 | `[~]` | 五个 AUI 区域已完成 host 包装；尚未运行布局、浮动、主题和窄屏回归 |
| RV-T04 | `[~]` | 页签自绘、无默认背景线、SidePanel 宿主适配已落地；窄栏溢出、浮动/分裂和真实截图仍待复核 |
| RV-T05 | `[~]` | 树/列表边框和 SVG 图标回退已落地；禁用态、DPI 与真实拖放截图待复核 |
| RV-T06 | `[~]` | 属性表单已接入圆角壳，FPGA 字段已统一高度/边框；IME、Tab、回填和窄栏截图待复核 |
| RV-T07 | `[~]` | 终端样式/selection/caret/空白区已显式绑定；历史文本和主题切换运行时对拍待复核 |
| RV-T08 | `[~]` | Windows 目标成功构建并记录新指纹；真实窗口场景和最终签收仍待执行 |

## 3. 任务状态

RV-T00～RV-T08 当前均为 `[~]`：对应实现已落地并成功编译，但真实窗口截图、交互和验收夹具仍未闭环。规划文件完整性或编译通过不等同视觉验收。

## 3. 实施记录模板

每批次追加：日期/实际负责人/任务ID；开工源码与配置快照；实际触达函数；命令/exit/产品身份；截图与量测；自动检查/真实操作结果；偏差与回退位置。不能从当前源码重新生成“改动前”基线。

| 验收项 | 状态 | 日志/截图 | 环境与限制 |
| --- | --- | --- | --- |
| RV-V01～RV-V12 | 未执行 | — | RV-T00 尚未完成截图/夹具闭环；RV-T02 尚未运行真实窗口验证 |

## 4. 继承问题与新缺陷分账

| 编号 | 类型 | 事实 | 当前处理 |
| --- | --- | --- | --- |
| RV-I01 | 继承 | T-420 最后一次452/33，未归因，见第一轮 §26/28 | 宿主改动后做真实画布操作；复用压力夹具需先补前台前置 |
| RV-I02 | 继承 | 多DPI真机/双屏/原生控件内部绘制尚未全量人工验证 | 本轮注明实机范围与具体例外，不虚报 |
| RV-I03 | 继承 | 原13项Agent测试未注册；完整工程/硬件业务验收有缺口 | 不把本轮UI通过数当这些业务通过；不扩大本轮到全业务矩阵 |
| RV-I04 | 继承 | 第一轮改造前冷启动/截图基线未采集 | 不补造；本轮 RV-T00 建新的本轮基线 |

新增缺陷使用 `RV-F01` 起编号，记录严重度、复现、代码归因、修复批次、重测结果；不预先登记虚构的产品故障。
