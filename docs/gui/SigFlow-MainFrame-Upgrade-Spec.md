# SigFlow 主界面升级规格

> 版本：v1.1 · 2026-10-09。状态：实施规格，未实现、未验收。  
> 来源：相邻 `SigFlow_FPGA/docs/gui/SigFlow-MainFrame-Upgrade-Spec.md` v1.0；本版按当前 `SigFlow_FPGA_Cmake` 工作区校订。  
> 执行清单：[task.md](task.md)。配套：[教育 Agent GUI spec](../agent/SigFlow-Edu-Agent-GUI-Spec.md)、[构建指南](../BUILD.md)。

## 1. 目标、范围与优先级

保留“左下属性区、独立 Pin Assign 窗口、移除旧 DeepSeek 页签、自由停靠、浅深主题”的设计目标。分为三个交付包：P1 布局迁移、P2 停靠与布局恢复、P3 统一主题。P1/P2/P3 的编号表示批次，不表示缺陷优先级。

本规格负责主窗口结构、菜单、面板生命周期、布局设置和视觉基础。编辑、图码同步、综合、仿真、引脚绑定、CST 导出、TraceBridge 的业务行为继续复用。新增文件加入 CMake 显式源列表；不新增第三方依赖，不升级 wxWidgets。

与教育界面的共同规则：

- `UiTheme`、AUI pane 注册、Window 菜单、布局持久化由本规格维护；Agent 面板复用，不再建设第二套。
- “移除右侧栏”指移除旧 `right_sidebar` 容器。教育版后续可在右侧新增 `agent_assistant`，不与 P1 冲突。
- `center_area` 保留为不可关闭、不可浮动的中央工作区；内容工具 pane 才开放拖动、四边停靠和浮动。
- 不将主窗口布局工程扩展成 Agent 后端、教育策略、执行授权或工程版自动化。

## 2. 当前实现锚点与迁移清单

以下是 2026-10-09 静态核对的符号；实施时重新搜索符号，不按原稿行号修改。工作区已有未提交代码改动，实施记录必须写实际 commit 或工作区标识。

| 资产 | 本仓库位置/锚点 | 迁移要求 |
|---|---|---|
| 左侧三页 | `MainFrame.cpp`：`leftSideNotebook`、sideBar 的 `wxEVT_TOOL` lambda | 三页及切换保持；pane 查询由窗口指针改为稳定名称 |
| 右侧三页 | `rightNotebook`、`Property`、`Pin Binding`、`pDeepSeek->CreatePanel` | 三项迁出/移除后才能删除容器 |
| 属性刷新 | `m_sfnPropertyPanel->LoadNode`、`PropertyLoadNode` | 保留所有选中刷新调用 |
| 引脚入口 | `DoFpgaPinBinding`、`MainMenuBar.cpp` 的 `wxID_HIGHEST + 234` | 保留校验、`LoadProject` 和事件 ID |
| 旧助手生命周期 | `Composer::LegacyAssistant()`、`SetProjectRoot` | 去除 UI 挂载，保留有业务用途的通知与判空 |
| 中央区 | `mainSplitter`：canvas / verilogEditor 上下分割 | 当前不是四视图 notebook；本轮保持结构 |
| 底部区 | `bottomNotebook`：Terminal / Waveform | 保留页签功能与当前 WavePanel 实例 |
| AUI | `topBar`、`sideNav`、`left_sidebar`、`right_sidebar`、`bottom_tabs`、`center_area` | 稳定 pane 名称；清除旧右栏查询 |
| 窗口菜单 | `RebuildWindowMenu` | 核对现有实现并扩展，避免重复 Bind |
| DPI 与 dock art | `ModernDockArt`、`wxEVT_DPI_CHANGED` 中的类型转换 | 迁移为新的唯一 art 类型及安全更新方式 |
| 关闭 | `MainFrame::~MainFrame` 的 `m_auiMgr.UnInit()` | 已有 UnInit；只保留一次且在子控件销毁前调用 |
| TraceBridge | `DoTraceBridge` 遍历 notebook；`main/debug/TraceBridgeWindow.*` | 审查对旧右栏/首个 notebook 的依赖，入口仍打开正确窗口 |
| 主题/构建 | `main/fpga/FpgaTheme.h`、根 `CMakeLists.txt` | `main/ui/` 尚未创建；采用 CMake/GCC，不采用原稿 MSVC 要求 |

## 3. P1：布局迁移

### P1-1 左栏属性区

`left_sidebar` 注册新的 `wxSplitterWindow`。上部承载原 `wxSimplebook` 三页；下部为带“属性 Properties”标题的容器，内部承载唯一的 `SFNPropertyPanel`。

- 使用 `SplitHorizontally`，初始上/下为 65%/35%，`SetSashGravity(0.65)`。首次可用尺寸确定后计算 sash；不能用构造时为零的高度。
- 左栏默认宽 260 DIP，上下窗格建议最小各 120 DIP。小窗口按可用空间缩放，不能以最小值挤掉中央区。
- 从旧 notebook 移出控件时使用不销毁页面的迁移方式；解除旧 sizer/page 归属后 Reparent，再注册新容器。禁止 `DeletePage` 销毁仍由成员持有的面板。
- sideBar 查询 `GetPane("left_sidebar")`，点击任一图标同时显示该 pane、切换上部页、更新 caption 与菜单勾选态。不能继续查询已不直接注册为 pane 的 `leftSideNotebook`。
- 属性编辑和 `LoadNode` 路径不变。未选择对象显示说明；选中失效对象清空，不能保留悬空指针。
- sash 比例单独持久化；比例越界、窗口太小或旧配置非法时回到默认。Reset Layout 同时重置此比例。

### P1-2 独立 Pin Assign

FPGA 菜单显示 `Pin Assign...`，事件 ID 仍为 `wxID_HIGHEST + 234`。建议新增 `main/fpga/FpgaPinAssignFrame.h/.cpp`，由 MainFrame 持有一个顶层窗口及原有唯一 panel。

- 首次有效工程操作创建窗口，默认 980×640 DIP；不足屏幕工作区时缩小并居中。
- `DoFpgaPinBinding` 先判空、校验工程、读取 top/source，再调用现有 `LoadProject`，最后 Show/Raise。重复菜单只聚焦同一窗口。
- 普通关闭 Hide，不销毁 panel；Window 菜单可重新显示。MainFrame 最终退出时解除回调并销毁子窗口一次，禁止旧 Hide 逻辑阻止应用退出。
- 切换/关闭工程时清理旧工程绑定状态并更新窗口标题；有未提交编辑时沿用面板已有保存/放弃行为，缺少行为则实施前补清楚，不能默默覆盖。
- 位置、尺寸与最大化状态写应用 `wxConfig`；恢复时检查当前显示器工作区。拔掉副屏后仍可见；保存普通窗口矩形，不把最大化矩形当普通尺寸。
- 窗口内加载、绑定、验证与 CST 导出沿用原逻辑。主界面不再出现 Pin Binding 页签。

### P1-3 旧右栏与 DeepSeek 页签

删除 `pDeepSeek->CreatePanel(rightNotebook)` / `AddPage` 的 UI 挂载。插件加载与工程生命周期通知按实际用途保留，不卸载插件、不修改插件 API。

属性和引脚面板迁移完成后，移除 `rightNotebook`、`right_sidebar` 的注册、尺寸修正、菜单项及其他查询。清查 TraceBridge 与插件回调，不能通过查找“第一个 notebook”误激活 Terminal。中央 canvas/RTL splitter 保持现有业务连接并获得空出的宽度。

P1 可先单独移除 DeepSeek 页签，但不得在剩余两个面板迁移前销毁右栏容器。

## 4. P2：停靠、菜单与布局

### P2-1 能力边界

采用当前 wxAUI 提供的移动、四边停靠、浮动、关闭与最大化。保留已有 manager flags，按需增加 `ALLOW_FLOATING`、`LIVE_RESIZE`、`ALLOW_ACTIVE_PANE`，避免整体覆盖 flags 丢失原能力。

| pane | 拖动/浮动/关闭 | 初始布局 |
|---|---|---|
| `topBar`、`sideNav` | 固定，不能关闭和浮动 | 顶部 / 最左侧 |
| `center_area` | 中央固定，不能关闭和浮动 | canvas + RTL |
| `left_sidebar` | 可移动、四边停靠、浮动、关闭、最大化 | 左侧 260 DIP |
| `bottom_tabs` | 同上，作为完整 pane 浮动 | 底部 Terminal + Waveform |
| 教育 pane | 采用相同策略，另受版本/模式可见性过滤 | 见 Agent spec |

`wxAUI_NB_TAB_MOVE/EXTERNAL_MOVE/TAB_SPLIT` 表示页签移动/跨 notebook 移动/分组，不构成页签拖出自动创建顶层浮窗的承诺。本期保证整个 pane 浮动，保留 notebook 内移动/分组；跨 notebook 转移须先验证 parent、业务成员和事件绑定安全，未经验证不得放开。单页顶层浮动及自动隐藏列为后续扩展。

本期不把 `PinButton(true)` 当作 auto-hide 实现，不展示没有行为的 pin 按钮。以 Close + Window 菜单重新显示满足收起/找回；自绘边缘自动隐藏栏需另立规格与验收。

### P2-2 Window 菜单

建立稳定的 pane name → 菜单 ID 映射，标签可本地化，业务定位不用 caption 或页签文字。动态重建清除旧条目并避免重复绑定。

- 枚举已注册的工具 pane，排除中央区和导航骨架；按版本、能力过滤教育项。
- 勾选态与 `IsShown` 一致；关闭、重开、拖动与模式变化后同步。浮动窗口关闭应隐藏 pane，不销毁业务控件。
- 加入 Pin Assign、Save Layout、Reset Layout。找回面板时将浮窗限制在屏幕内。
- Reset Layout 先确认布局变更（无需清除工程），恢复默认 pane、左栏 sash、Pin Assign 默认几何；不重建业务对象、清空 Job 或重启 Agent。

### P2-3 布局持久化与兼容

统一使用现有应用 `wxConfig`，不新增另一份工程内 `layout.json`。建议键：`Ui/Layout/v2/<edition>/<mode>/Perspective`、`LeftSashRatio`、`PinAssign/Rect`，另存布局格式版本。`mode` 为 learn/workspace；没有教学功能时只使用 workspace。

1. 所有适用 pane 注册后，捕获本版本默认 perspective；恢复用户配置，再重施中央区/导航骨架和版本可见性约束。
2. Save Layout 和正常退出写配置；退出保存发生在 `UnInit` 前。设置写入失败提示“布局未保存”，不阻止关闭。
3. 旧配置含 `right_sidebar`、未知 pane、格式错误或加载失败时忽略失效项并保留新默认；恢复后检查中央区、工具 pane 可找回、浮窗在屏幕内。无法安全迁移则整份回退默认，不能留下半加载状态。
4. 使用格式版本及 pane 清单识别变更；关闭 Agent 功能后不得复活教育 pane。旧配置保留备份直到新配置成功写入。
5. 只保存布局与界面偏好，不保存工程状态、token、grant 或会话。切换工程无需重新创建 pane。

## 5. P3：统一主题、字体与 DPI

### P3-1 共享主题模块

新增 `main/ui/UiTheme.h/.cpp`，以语义 palette 和主题设置管理 light/dark。主题偏好为 `system | light | dark`，首次默认 `system`，无法检测系统时 light；用户显式选择后系统变化不覆盖选择。模式、布局和主题设置分别存储。

统一 token 名称如下，Agent spec 使用同一集合；禁止两篇 spec 定义两套同名色值。

| token | light | dark |
|---|---|---|
| bgWindow / bgPanel / bgSunken | `#FAFAF8` / `#FFFFFF` / `#F1EFE8` | `#1E1E20` / `#26262A` / `#2D2D32` |
| hairline | `#E5E5E5` | `#3A3A40` |
| text / textSecondary / textMuted | `#1D1D1F` / `#5F5E5A` / `#6E6D67` | `#F5F5F7` / `#B6B6BE` / `#A1A1AA` |
| accent / accentText | `#007AFF` / `#0C447C` | `#78B7FF` / `#BEDCFF` |
| guideBg / guideText | `#E6F1FB` / `#0C447C` | `#21364D` / `#BEDCFF` |
| doneBg / doneText | `#EAF3DE` / `#27500A` | `#273C2B` / `#BFE3AD` |
| hardwareBg / hardwareText | `#FAEEDA` / `#712B13` | `#463226` / `#FFD0A4` |
| errorBg / errorText | `#FCEBEB` / `#791F1F` | `#472A30` / `#FFC1C1` |
| success / warning / error | `#26813D` / `#A35A00` / `#C73838` | `#74D38A` / `#FFC36B` / `#FF8D8D` |

浅色原稿的最弱文字色加深，以改善小字号可读性；色彩同时配状态词/图标，不以颜色作为唯一信息。发丝线为 1 DIP，绘制时对齐设备像素；不承诺跨平台物理 0.5px。

### P3-2 字体与公共组件

- 尺寸统一按 DIP：标题 14、正文 12、辅助 11、最小 10；禁止将原稿 `Font(9)` 注释成 12px。
- `wxFont` 构造的字号通常为 point；实现使用适用的 pixel-size 接口设置 `FromDIP` 后的高度，或明确采用 point 并校准实际高度，不混用 point/DIP。
- regular/medium 两档，不支持 medium 则 normal。使用系统 GUI 字体，确保中文回退；新增代码不强制全局粗体。
- 公共卡片圆角 10 DIP，按钮 6–8 DIP、间距 8/12/16 DIP。原生标题栏、菜单、滚动条允许系统外观；不为视觉统一重写系统控件。
- 使用现有图标资产、`wxBitmapBundle` 及支持的 SVG 加载方式；核对实际 `main/res` 资源目录与打包规则，不新增无来源的图标路径。

### P3-3 AUI art 与现有主题兼容

新增或抽出 `UiDockArt`、`UiTabArt`；移除渐变，统一 caption、sash、字体、激活页强调线。实现以本仓 wx 头文件的公开 API 为准，不照抄原稿未确认的 `wxAUI_TABART_TAB_SIZE` 或通用 `SetMetric`。

当前 `ModernDockArt` 与 DPI 回调中的强制类型转换必须一并适配，避免替换 provider 后访问错误类型。provider 所有权按 wxAUI 规则移交，不能重复 delete。主题回调注册返回可解绑句柄，窗口销毁时解除，关闭浮窗后切主题不能访问旧控件。

`FpgaTheme` 先做兼容适配，逐项迁移本轮覆盖的控件；其余原有工具主题可后续迁移，但必须记录剩余范围。禁止删除仍被引用的 helper 或将整套 FPGA 工具样式重构夹带在 P1。WaveTheme 的信号/轨道语义色保留，通过适配接收背景与文字 token。

### P3-4 主题更新和降级

切换时刷新已注册面板、AUI art、notebook art、Pin Assign 和后续教育 pane；保持选中对象、文本、焦点、页签与布局。处理 `wxEVT_SYS_COLOUR_CHANGED` 和 DPI 变化。原生控件不支持定制颜色时接受原生显示，但文本必须可读。

动效可完全关闭；有动效时用主线程 timer、自绘和局部刷新，150–200ms，尊重减少动画偏好。禁止为呼吸灯持续全窗口重绘；本期运行状态静态图标即可。

## 6. 非功能与验收

| 编号 | 验收场景 | 通过条件 |
|---|---|---|
| MF-AC01 | 左栏切三页、选择/编辑元件属性 | 上部切换、下部刷新正确；隐藏后图标找回；无旧指针查询 |
| MF-AC02 | Pin Assign 首开/重开/切工程/退出 | 单实例，绑定和 CST 导出不回归；正确工程；退出无遗留窗口 |
| MF-AC03 | DeepSeek 有/无插件，两种启动 | 无旧页签/旧右栏；插件为空安全；生命周期通知仍正确 |
| MF-AC04 | 左栏、底部 pane 浮动与四边停靠 | 整 pane 可关闭/找回/最大化；中央区与导航不能丢失 |
| MF-AC05 | 布局保存、重启、Reset、旧/损坏配置 | 恢复或完整回到默认；业务对象未重建；无不可达面板 |
| MF-AC06 | 副屏移除、窗口缩小、不同 DPI | 浮窗均在工作区内；中文不截断；主编辑区可用 |
| MF-AC07 | light/dark/system、关闭浮窗后切换 | 明暗一致、控件可读；焦点/状态保持；无失效回调 |
| MF-AC08 | New/Open/Save、图码、仿真/综合、波形、TraceBridge | 原入口和业务行为仍可使用；不以截图代替功能操作 |
| MF-AC09 | edu/pro/dev 与 Agent 编译开关开/关 | 适用 pane 和菜单正确；不支持能力不显示可执行入口 |

GUI 手工矩阵至少覆盖 Windows 100%/150%/200% 缩放、1280×720 和 1920×1080；原生 Linux 记录实际桌面/wx 版本。切主题、拖 pane、关闭/找回各循环 20 次，无崩溃、重复绑定或窗口累积。布局/主题操作不得启动 EDA/Agent 请求。

每批次按 [BUILD](../BUILD.md) 使用 CMake/GCC 构建并执行相关现有回归；新增纯数据布局迁移逻辑有正负用例，GUI 不要求为每个样式 setter 写测试。证据记录环境、命令、结果和截图；未执行平台标为未执行。

## 7. 交付及修订记录

交付物为 P1/P2/P3 对应代码、CMake 登记、配置迁移、验收记录及本目录 task 状态。实施记录放 `acceptance.md`（实施时创建），完成项同时给 commit/工作区、构建命令、手工步骤和结果。

v1.1 修订：替换过期行号与 MSVC 验收；补左栏事件路由、Pin 生命周期、旧 perspective 迁移；明确 pane 浮动与 notebook 页签能力边界；统一主题默认值和字号单位；限定 auto-hide/单页浮窗为后续；与 Agent spec 共享主题、pane 与布局基础。
