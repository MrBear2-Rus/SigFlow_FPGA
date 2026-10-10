# WavePanel 波形面板对标升级 TODO List

> 参考实验品：`E:\EDA_Race\TEST1 - 1\TEST1`（Bear2Wave，GTKWave 风格波形查看器，wxWidgets + OpenGL）  
> 关联设计：`docs/SigFlow-TraceBridge-Design.md` 第 20 节（规划）与第 21.9 节（主 TODO）  
> 文档状态：规划稿（独立跟踪波形面板升级任务）

## 1. 背景与现状

SigFlow 现有波形面板（`main/WavePanel.cpp` / `main/WaveformPanel`）仅支持：打开 VCD、按行绘制信号波形、缩放/重置、时间滑杆与播放、鼠标悬停 tooltip、信号颜色分配。

升级目标（对照实验品 Bear2Wave）：

1. 大仿真文件可用：VCD 懒加载、侧车索引、内存预算，视口按需读跳变。
2. 交互体验对齐主流查看器：GL 渲染、缩放平移、Marker/A-B 测量、模式搜索、信号树懒加载。
3. 双轨比较：仿真/重放 vs 硬件双轨同屏、联动播放头、差异标记（TraceBridge 核心需求）。
4. 调试联动：UART 协议 lane、事件时间线、RTL/SFTree 双向跳转、会话保存恢复。

## 2. 对标能力清单

| 能力 | 实验品位置 | 移植方式 | 优先级 |
| --- | --- | --- | --- |
| OpenGL 批量渲染 + 文字层合成 | `panels/WaveformGLRenderer.*`、`panels/WaveformPainter.*` | 重写适配（GL 顶点批次 + DirectWrite/wxBitmap 文字纹理） | P1 |
| 大文件懒加载 | `trace_loader.*`、`vcd_lazy.*` | 视口增量加载 + 后台线程 + 取消 | P1 |
| 侧车索引 `.bwidx` | `trace_sidecar_idx.*` | 格式借鉴，独立实现 | P1 |
| 内存预算 LRU | `trace_memory_budget.*` | 独立实现 | P1 |
| 多格式读取（FST/VZT/LXT2/GHW） | `fst_loader.*`、`lxt*_loader`、`vzt_loader`、`ghw_loader` | vendor 读库（先许可证评估） | P2 |
| 模块树 + 虚拟信号列表 | `SignalModuleTree.*`、`core/module_tree_lazy.*` | 重写适配 | P1 |
| 模式搜索 | `core/pattern_search.*`、`ui/PatternSearchDialog.*` | 算法移植 | P1 |
| Marker / A-B 测量 / 本地统计 | `ui/MainFrameMarkers.*`、`core/waveform_analysis.*` | 重写适配 | P1 |
| Compare 双窗/双轨联动 | `ui/WaveformCompareHub.*` | 重写适配（联动播放头/视口/平铺） | P1 |
| 协议 lane（I2C/SPI/UART） | `ui/ProtocolLanePanel.*` | 首版仅 UART，重写适配 | P2 |
| 会话保存/恢复（.bwv） | `core/WaveformSession.*`、`WaveformSessionController.*` | 格式借鉴，独立实现 | P1 |
| RTL 源浏览与信号树联动 | `ui/RtlSourcePanel.*`、`core/rtl_parser.*` | 与 SigFlow SFTree 结合 | P1 |
| 主题切换 | `core/ui_theme.*` | 与 FpgaTheme 统一 | P2 |

## 3. 阶段划分与阶段门禁

### W1 数据层（可独立验收）

**阶段门禁**：加载 1 GB 级 VCD 首屏 < 2 s、内存峰值可控、滚动不卡顿；纯数据接口，无 UI 依赖。

### W2 渲染层（依赖 W1）

**阶段门禁**：10 万跳变视口 60 FPS 缩放平移；远程桌面/无 GL 环境自动回退 wxDC 且功能不降级。

### W3 交互层（依赖 W2）

**阶段门禁**：信号树搜索、Marker/A-B、模式搜索、Compare 联动、会话保存恢复全部可用，快捷键与右键菜单补齐。

### W4 集成层（依赖 W3 + TraceBridge P3/P4）

**阶段门禁**：打开 `capture.vcd`、加载 `compare.json` 高亮首差异、事件时间线跳转、双击差异跳转 RTL/SFTree，全部走通。

## 4. TODO List

### W1 数据层

**T-W1-01 统一 trace 加载 API（[P1]）**

- [x] 定义统一的信号树 / 时间轴 / 跳变缓存接口（`main/trace/TraceSource.h`，对标实验品 `trace_load_signals` 风格）。
- [x] 支持按视口范围增量读取跳变（`Query(signal, t0, t1)` 窗口查询，视口 ± 边距预取由 W2 使用）。
- [x] 后台查询 + 可取消（`TraceQueryService`，视口边距预取，完成/取消/错误状态区分）。
- [ ] 验收：1 GB VCD 打开后首屏 < 2 s；拖动/缩放不触发全量加载。

**T-W1-02 VCD 懒解析与紧凑存储（[P1]）**

- [x] VCD 头部解析（模块层次、信号表）与跳变解析分离（`VcdLazyTraceSource::ScanHeader`）。
- [x] 跳变紧凑存储（每跳变仅存时间+值，`Transition`）。
- [x] 多 bit 总线按需解包为位向量。
- [x] 验收：跳变查询从时间索引最近采样点 seek 后局部解析，复杂度与视口相关（smoke 测试覆盖窗口查询正确性）。

**T-W1-03 侧车索引 `.bwidx`（[P1]）**

- [x] 定义侧车索引格式（`TraceSidecarIndex`，含时间索引与信号表，版本化）。
- [x] 首次打开生成索引，源文件大小/mtime 变化时重建（增量更新检测）。
- [x] 索引损坏/版本不符时回退全量扫描并重建。
- [x] 验收：二次打开同一 VCD 直接加载侧车，不再全量扫描（smoke 测试 `HasSidecar` 验证）。

**T-W1-04 内存预算 LRU（[P1]）**

- [x] 跳变缓存 LRU 淘汰与内存上限配置（`TraceMemoryBudget` + `CachingTraceSource` 装饰器）。
- [ ] 内存占用统计展示（调试面板，属 W3 UI 层）。
- [x] 验收：缓存字节数不超过上限、命中/淘汰正确（smoke 测试覆盖）。

**W1 实现记录**

- 已新增 `main/trace/`：`TraceTypes`、`TraceSource`（统一接口）、`VcdLazyTraceSource`（懒解析 + 时间索引 + 侧车）、`TraceSidecarIndex`（.bwidx）、`TraceMemoryBudget`（LRU 字节预算）、`TraceCache`（查询缓存装饰器）。
- 已接入 main.vcxproj / filters（trace 分组），main 工程构建 0 错误 0 警告。
- 测试：`tests/wave/TraceLazySmoke.cpp`（ALL PASS），覆盖头部/信号/时间范围/窗口查询/ValueAt/远窗口 seek/侧车复用/预算淘汰/缓存命中。
- 未完成项：1 GB 级性能验收（W2 阶段门禁测量）、`T-W1-05` 多格式读库（P2，许可证评估先行）。

**T-W1-05 多格式读库（[P2]）**

- [ ] 许可证评估（GTKWave 读库 GPL，先出结论再 vendor）。
- [ ] vendor FST 读库并实现 loader。
- [ ] VZT / LXT2 / GHW loader 视需要引入（对外部文件提供格式检测与错误提示）。

### W2 渲染层

**T-W2-01 OpenGL 渲染框架（[P1]）**

- [x] GL 上下文创建与 wxGLCanvas 集成（`WaveformGLCanvas`，运行时失败自动回退软件）。
- [x] 渲染管线：GL 1.1 立即模式绘制线段批次（VBO 需 wglGetProcAddress 加载，留作后续性能优化）。
- [x] 每帧仅渲染视口窗口内的跳变（数据层窗口查询天然按视口裁剪）。
- [ ] 验收：10 万跳变视口下缩放/平移 60 FPS。

**T-W2-02 文字层合成（[P1]）**

- [x] 信号名 / 时间轴 / 值标注文字层（`WaveformTextLayer`：wxBitmap → RGBA）。
- [x] 文字层上传为 GL 纹理并与波形层合成，消除 wxDC+SwapBuffers 闪烁。
- [ ] 验收：高 DPI 下文字清晰、无闪烁。

**T-W2-03 视口交互（[P1]）**

- [x] 缩放（滚轮以光标为中心 / `+` `-`）、平移（拖动 / 左右键）、翻页（Home/End）。
- [x] 边沿跳转（`,` / `.`）、回到首/末（Home/End）。
- [x] 快捷键列表：滚轮缩放、拖动平移、`+/-` 缩放、`←/→` 平移、`Home/End`、`,`/`.` 边沿跳转（待 W3 补菜单与帮助入口）。

**T-W2-04 wxDC 软件渲染回退（[P1]）**

- [x] GL 上下文失败时回退 wxDC 绘制（`OnGlFailed` → 销毁 GL 画布 → 软件模式）。
- [x] 两套渲染共享同一数据层与交互逻辑（`WaveViewState` / `BuildWaveformFrame` / `WaveViewInteraction`）。
- [x] 验收：回退模式功能完整（smoke 软件模式 PASS）；帧率 ≥ 15 FPS 待人工测量。

**W2 实现记录**

- 新增 `main/wave/`：`WaveViewState`（共享视口状态）、`WaveformRenderData`（两后端共用的帧构建）、`WaveformGLRenderer`（GL 1.1 线段绘制 + 文字纹理叠加）、`WaveformGLCanvas`（wxGLCanvas 后端）、`WaveformView`（主控件，软件回退 + 交互）、`WaveformTextLayer`（文字层位图）。
- 已接入 main.vcxproj / filters，main 工程构建 0 错误 0 警告。
- 测试：`tests/wave/WaveViewSmoke.cpp`（GL 与软件双模式均 PASS，覆盖缩放/平移/边沿跳转/状态校验）；`tests/wave/WaveformRenderDataSmoke.cpp`（帧构建与视口状态，PASS）。
- 过程中修复的两个问题：① GL 画布必须引用父视图的 trace 源（按值拷贝在 `SetTraceSource` 前为空，导致解引用崩溃）；② 本环境 `wxAutoBufferedPaintDC` 构造崩溃，软件回退改用 `wxPaintDC`（后续再评估手动双缓冲）。
- 未完成项：60 FPS 与高 DPI 视觉验收（需人工/基准测量）、VBO 批处理性能优化（wglGetProcAddress）、菜单与帮助入口（W3）。

### W3 交互层

**T-W3-01 模块树与信号浏览（[P1]）**

- [x] 模块层次树 + 信号列表（`TraceViewPanel` 内 `wxTreeCtrl`，按 scope 分层）。
- [x] 搜索过滤（树内按名字/全路径过滤）、双击添加/移除信号。
- [x] 右键菜单（添加/移除、别名、注释）。
- [x] 模块节点默认折叠，展开时懒创建子模块和信号节点；搜索时 materialize 命中项。
- [ ] 与 SFTree 数据互通（信号路径双向解析）。
- [ ] 验收：千级模块、万级信号工程下展开与搜索流畅。

**T-W3-02 Marker 与测量（[P1]）**

- [x] 命名 Marker（Shift+点击）、A/B 区间 ΔT 测量（Ctrl+拖动），软件/GL 双后端绘制。
- [x] 本地统计：边沿数、周期、占空比、X/Z、毛刺（`WaveAnalysis::MeasureSignal`，毛刺阈值可配置）。
- [ ] 验收：测量值与测试向量人工核对（纯数据测试已覆盖数值正确性）。

**T-W3-03 事件时间线（[P1]）**

- [x] 事件列表（`WaveEvent`，来源可接 TraceBridge compare.json），事件在波形中以彩色竖线+标签显示。
- [x] 事件行点击 → 跳转 + 播放头；播放头变化 → 事件列表高亮最近事件。
- [ ] 验收：事件与波形双向跳转延迟 < 100 ms。

**T-W3-04 模式搜索（[P1]）**

- [x] 多信号模式规格（rise/fall/any/high/low/value）前后向搜索（`WavePatternSearch`，语义对标 Bear2Wave pattern_search）。
- [x] 面板搜索框 + Prev/Next 跳转（`name:kind[:value]` 语法，逗号分隔多条件）。
- [ ] 搜索结果列表、命中计数与循环跳转 UI（当前为单次跳转）。
- [x] 验收：纯数据测试覆盖上升沿/组合条件/任意边沿，结果正确。

**T-W3-05 协议 lane（[P2]）**

- [x] UART 解码 lane（首版：full COBS/CRC 与 minimal 固定帧、字节偏移、错误和残帧显示，服务于 TraceBridge 回读帧）。
- [ ] I2C / SPI 解码（对标 `ProtocolLanePanel`），事务层标签与跳转。【未开始】
- [ ] 验收：对已知 UART/I2C 测试向量解码结果正确。

**T-W3-06 Compare 双轨联动（[P1]）**

- [x] 双窗/双轨联动：时间窗与播放头广播（`WaveCompareHub`，可分别开关）。
- [ ] 平铺排列、主题同步、菜单勾选状态同步。
- [x] 首差异标记与事件高亮（数据来自 compare.json，板上回环到波形呈现已验收）；差异区域连续着色待候选图接入。
- [x] 验收：smoke 验证一个视图改变时间窗后另一视图严格同步。

**T-W3-07 会话保存/恢复（[P1]）**

- [x] 会话格式（`.bws`，JSON）：源路径、信号列表、视口、Marker、事件、A-B、播放头。
- [x] 保存/读取（面板工具栏），smoke 覆盖保存→加载→应用往返一致。
- [ ] 自动恢复最近会话；radix/别名/注释行字段。
- [ ] 与 TraceBridge 会话目录合并导出/导入。
- [x] 验收：smoke 验证保存后重新应用，显示现场与标记完全恢复。

**T-W3-08 主题（[P2]）**

- [ ] 深色/浅色主题切换，应用到所有波形窗口。【未开始，P2】
- [x] 与 `FpgaTheme` 配色统一（当前固定深色配色与 FpgaTheme 一致）。

**W3 实现记录**

- 新增：`WaveViewState` 扩展（播放头/Marker/事件/A-B）、`WaveAnalysis`（A-B 统计）、`WavePatternSearch`（多信号模式搜索）、`WaveSession`（.bws 会话 JSON）、`WaveCompareHub`（双窗联动）、`TraceViewPanel`（W3 宿主面板：信号树+搜索、波形视图、事件列表、模式搜索、会话工具栏、测量标签）。
- 交互：Shift+点击放 Marker、Ctrl+拖动 A-B、单击放播放头、Esc 清除 A-B/播放头；软件与 GL 双后端均绘制覆盖层与标签。
- 已接入 main.vcxproj / filters，main 工程构建 0 错误 0 警告。
- 测试：`WaveViewSmoke`（GL/软件双模式 PASS：markers/events/measure/session/compare 联动全部通过）、`WaveAnalysisSmoke`（测量+模式搜索 ALL PASS）、`WaveformRenderDataSmoke`、`TraceLazySmoke` 保持通过。
- 未完成（见上方 [ ]）：I2C/SPI lane、右键菜单/虚拟列表/SFTree 互通（W3-01 部分）、搜索结果列表（W3-04 部分）、平铺/菜单同步（W3-06 部分）、会话自动恢复（W3-07 部分）。

### W4 集成层

**T-W4-00 MainFrame 接入 TraceViewPanel（[P1]）**

- [x] MainFrame 的 `m_wavePanel` 从旧 `WavePanel` 切换为 `sigflow::wave::TraceViewPanel`。
- [x] TraceBridgeWindow 的 `hwWavePanel_` / `simWavePanel_` 同步切换为 `TraceViewPanel`。
- [x] 所有 `OpenVCDFile(wxString)` → `OpenTrace(std::string)`；`SetProjectPath` → `SetSessionDir`。
- [x] 编译 0 error；DEBUG CI 11/11 ALL PASS。
- **效果**：W1-W3 已实现但未接入的能力全部激活——懒加载、侧车索引、内存预算 LRU、信号树、OpenGL 渲染（wxDC 回退）、Marker/A-B 测量、模式搜索、会话保存、Compare 联动。

**T-W4-01 TraceBridge 数据接入（[P1]）**

- [x] 直接打开 `capture.vcd`（复用 W1 数据层 VcdLazyTraceSource + CachingTraceSource）。
- [x] `TraceViewPanel::InjectCompareEvents()` 将对齐锚点（绿）/每信号首差异（红）/摘要（橙）注入 `WaveViewState.events`，排序后 SetEvents 推给波形。
- [x] `SaveCompareJson()` 把比对结果写入会话目录 `compare.json`（alignment + firstDiffs + summary）。
- [x] `ShowComparisonResult` 把 HW/Sim 面板注册进 WaveCompareHub，开启 `s_linkTimeView` 和 `s_linkPlayheads` 联动。
- [x] 比对后播放头自动跳到首个差异（有差异）或对齐锚点（无差异），双面板分别按各自时间戳。
- [x] 采集完成时从 VCD 路径提取 `lastSessionId_`，供保存 compare.json 定位会话目录。
- [x] 编译 0 error；DEBUG CI 11/11 ALL PASS。
- **验收**：一次 TraceBridge 会话的 capture + compare 可完整还原显示，事件列表可跳转，双面板联动。

**T-W4-02 RTL/SFTree 双向定位（[P1]）**

- [x] 双击信号 → RTL 行 / SFTree 节点 / 画布元素（映射存在时）。
- [x] 从 RTL/SFTree 选择信号 → 添加至波形。
- [x] 首差异信号一键跳转上游候选（联动候选图）。
- [x] 数据验收：tracebridge_priority_smoke；GUI 集成由 SigFlow.sln Debug x64 构建验证。

## 5. 依赖与里程碑

| 里程碑 | 依赖 | 建议周期 | 交付物 |
| --- | --- | --- | --- |
| W1 数据层 | 无 | 1–2 周 | trace 加载 API、VCD 懒加载、索引、内存预算 |
| W2 渲染层 | W1 | 1–2 周 | GL 渲染 + 文字合成 + 视口交互 + 回退 |
| W3 交互层 | W2 | 1–2 周 | 信号树、Marker、搜索、Compare、协议 lane、会话 |
| W4 集成层 | W3、TraceBridge P3/P4 | 1 周 | capture/compare 接入、双向定位 |

## 6. 风险与许可证

- 实验品为独立工程：按"能力移植 + 重写适配"处理，不整目录复制。
- FST/LXT/VZT/GHW 读库来自 GTKWave（GPL）：vendor 前必须完成许可证评估；VCD 解析与自研渲染不受影响。
- OpenGL 在低端/远程桌面可能不可用：保留 wxDC 回退路径。

- 侧车索引与 `.bwv` 会话格式为自研格式：需保证向后兼容与版本化。

## 7. 2026-08-30 选定范围收口

- UART lane 已完成首版：full COBS/CRC 和 minimal 固定帧均可自动识别，支持帧详情、偏移、错误和残帧显示。
- RTL/SFTree/画布双向定位已完成数据与 GUI 接口接入，首差异及上游候选可回跳；无映射时保留明确的未找到状态。
- 本轮不推进其他未选定项目；I2C/SPI、多格式 trace、虚拟列表和会话归档继续保留在原 TODO。
