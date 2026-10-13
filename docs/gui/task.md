# 主界面升级执行清单

> 版本：v1.0 · 2026-10-09。依据：[主界面 spec v1.1](SigFlow-MainFrame-Upgrade-Spec.md)。  
> 状态：全部待实施；本文生成不代表 GUI 已修改。跨目录依赖：[Agent task](../agent/task.md)。

## 1. 执行与完成规则

`[ ]` 待做，`[~]` 进行中，`[x]` 完成且验证，`[!]` 阻塞。每项指定一名实际负责人，完成时在本项追加 commit/工作区标识、构建/测试命令、结果、截图或演示链接。阻塞写需要的输入及解除条件；禁止把历史测试结果当作本轮证据。

以下角色为责任建议，开工时映射到实际成员：GUI（窗口/布局）、UI（主题）、集成（构建/回归）。优先级“必须”表示本规格交付要求；“后续”不能阻塞首批，也不能宣称已实现。不改无关工作区内容。

## 2. 顺序与交付门槛

| Gate | 任务 | 签收结果 |
|---|---|---|
| MG0 基线 | GUI-00 | 当前布局、业务入口、构建与配置基线可复现 |
| MG1 布局 | GUI-01 → GUI-02/03 → GUI-04 | 属性左下、Pin 单窗口、旧右栏消失，业务回归通过 |
| MG2 停靠 | GUI-05 → GUI-06/07 | 整 pane 停靠/找回，布局安全持久化/迁移 |
| MG3 主题 | GUI-08 → GUI-09/10 → GUI-11 | 共享 theme/art、明暗/DPI、旧组件兼容 |
| MG4 集成 | GUI-12 | MF-AC01～09 有证据，适用平台与构建组合清楚 |

GUI-08 的纯主题模块可在 MG1 后先交付供 Agent 复用；MG3 整体仍需 MG2。不得为了先交付主题模块将 MG1/MG2 标为完成。

## 3. 任务明细

### GUI-00 当前基线与实施范围｜必须｜集成｜依赖：无｜spec §1/2

- [ ] 记录当前代码/未提交改动、CMake/GCC/wx 版本与有效 build 目录；按 BUILD 验证基线，不套用原仓库 MSVC 命令。
- [ ] 核对 pane、sideBar lambda、属性刷新、Pin/TraceBridge 入口、插件生命周期和析构符号；记录 UI 截图及当前功能结果。
- [ ] 记录原配置保存位置，备份布局/窗口几何；明确本轮只修改界面基础。
- [ ] 给 GUI/UI/集成指定负责人，确认与 Agent task 的共享文件归属。

完成条件：`acceptance.md` 含环境、现有失败项和锚点清单；MF-AC08 有改造前结果。

### GUI-01 移除旧助手 UI 挂载｜必须｜GUI｜依赖：GUI-00｜spec P1-3

- [ ] 移除 `CreatePanel(rightNotebook)` 与 DeepSeek AddPage；判空检查保留。
- [ ] 核对 `LegacyAssistant` / `SetProjectRoot` 的实际业务用途，保留必要通知；记录删除理由。
- [ ] 此时保留承载 Property/Pin 的旧容器，避免提前销毁。

完成条件：MF-AC03 中有/无插件启动均通过，原两个业务面板仍可用。

### GUI-02 左下属性区及侧栏路由｜必须｜GUI｜依赖：GUI-01｜spec P1-1

- [ ] 新建左 splitter、属性标题容器，安全移出/重挂唯一 Property panel 与三页 simplebook。
- [ ] 注册容器为 `left_sidebar`，改 sideBar 的 pane 查询为稳定名称；Show/切页/caption/勾选一致。
- [ ] 首次布局后计算 65/35 sash；处理小窗口和选中节点失效，保留 LoadNode/属性编辑路径。
- [ ] 实测三图标、canvas/tree 选中、编辑属性、隐藏/找回、缩放与工程重开。

完成条件：MF-AC01 通过，无被销毁的成员控件和失效 pane 查询；sash 保存接 GUI-07。

### GUI-03 Pin Assign 单例窗口｜必须｜GUI｜依赖：GUI-01｜spec P1-2

- [ ] 新建 `FpgaPinAssignFrame.h/.cpp` 并登记 CMake；唯一 panel 安全迁移，菜单 ID 保持。
- [ ] 修改 DoFpgaPinBinding：工程校验/LoadProject 后 Show/Raise；窗口关闭 Hide，宿主退出时正确销毁。
- [ ] 切工程/关工程时刷新上下文，处理未提交编辑；Window 菜单找回由 GUI-06 接入。
- [ ] 在现有 wxConfig 中保存几何/最大化状态，恢复时限于屏幕工作区。
- [ ] 验证重复打开 20 次单实例、CST 导出、无工程拒绝、切工程与主窗口退出。

完成条件：MF-AC02 通过，主窗口无 Pin 页签，副屏失联窗口仍可见。

### GUI-04 删除空右栏与入口清查｜必须｜GUI｜依赖：GUI-02、GUI-03｜spec P1-3

- [ ] 移除 rightNotebook、right_sidebar 注册、BestSize 调整、菜单项与残留查询。
- [ ] 检查 DoTraceBridge 的 notebook 遍历与插件回调，修正依赖旧右栏的显示路径；只改界面路由。
- [ ] 保留 center_area canvas/RTL splitter、bottom_tabs 与业务实例，验证新增空间正确分配。
- [ ] 构建并完成 New/Open/Save、图码、属性、Pin、Terminal/Waveform、TraceBridge 入口回归。

完成条件：MG1 签收；MF-AC01/02/03/08 通过。

### GUI-05 pane 停靠策略｜必须｜GUI｜依赖：GUI-04｜spec P2-1

- [ ] 保留已有 manager flags，启用当前 wxAUI 的 pane 移动、浮动、四边停靠、关闭/最大化。
- [ ] 固定 topBar、sideNav、center_area；工具 pane 按 spec 策略登记，后续教育 pane 走同一入口。
- [ ] 保留 notebook 内移动/分组；核查跨 notebook 迁移的 parent/事件后决定是否启用并记录结果。
- [ ] 禁止展示无行为 pin/auto-hide；不把 external move 当作单页浮窗实现。

完成条件：MF-AC04 通过，至少左栏/底部整 pane 可浮动与重新停靠，导航和中央工作区不会被关闭。

### GUI-06 Window 菜单及找回｜必须｜GUI｜依赖：GUI-05、GUI-03｜spec P2-2

- [ ] 建立 name→菜单 ID 映射；动态重建无重复 Bind，按版本过滤教育 pane。
- [ ] 同步关闭、显示、浮动关闭、模式切换后的勾选态；加入 Pin Assign/Save Layout/Reset Layout。
- [ ] 找回离屏浮窗，Reset 不重建业务面板；重置确认只针对布局。
- [ ] 循环关闭/找回 20 次，验证一个点击仅触发一次处理。

完成条件：MF-AC04/05 的关闭找回部分通过，所有可关 pane 有恢复入口。

### GUI-07 版本化布局持久化｜必须｜GUI｜依赖：GUI-06｜spec P2-3、P1-1

- [ ] 在 wxConfig 实现格式版本、edition/mode perspective、默认布局捕获、sash 比例与 Pin 几何保存。
- [ ] 注册完 pane 再恢复；恢复后重新施加固定 pane/版本能力约束。
- [ ] 迁移含 right_sidebar 的旧配置，处理未知 pane、坏格式、半加载、不可写配置与副屏移除；必要时完整回默认。
- [ ] 退出前保存、UnInit 保留一次；Reset 同时重置 pane/sash/Pin 几何且不清业务状态。
- [ ] 为配置解析/迁移/几何限界等纯逻辑补正负用例；执行手工重启和 Reset。

完成条件：MG2 签收；MF-AC05/06/09 通过，Agent 能复用模式布局保存，不出现第二份 layout.json。

### GUI-08 UiTheme 公共基础｜必须｜UI｜依赖：GUI-04｜spec P3-1/2

- [ ] 创建 `main/ui/UiTheme.h/.cpp` 并登记 CMake，落实 spec 唯一 palette、system/light/dark 偏好及 wxConfig 键。
- [ ] 统一 font 高度单位、中文系统字体、DIP 间距、卡片/按钮公共样式；unsupported medium 回退 normal。
- [ ] 提供可解绑的主题订阅与 DPI 更新入口，窗口销毁清除注册；给 Agent 面板提供稳定 API。
- [ ] 确认两个 spec 使用同一 token，Agent 不新增同名 theme。

完成条件：模块编译、浅深 palette 可切换，字号在 150% 下符合层级；API 和最小用法记入 acceptance.md。

### GUI-09 Dock/Tab art 与 DPI 回调｜必须｜UI｜依赖：GUI-08、GUI-05｜spec P3-3

- [ ] 使用当前 wx 公开 API 实现/迁移 UiDockArt、UiTabArt，统一无渐变 caption、sash、激活 tab。
- [ ] 适配 ModernDockArt 与 DPI 强转，明确 provider 所有权，更新已注册的 notebook。
- [ ] 处理 DPI/主题变化、活动/非活动/浮动/关闭按钮、长中文标签及键盘焦点绘制。

完成条件：MF-AC06/07 的 art 部分通过，变更 DPI 无错误类型访问和重复释放。

### GUI-10 既有控件与主题兼容｜必须｜UI｜依赖：GUI-08、GUI-09｜spec P3-3/4

- [ ] 适配 FpgaTheme，迁移本轮涉及的主框架、左栏属性区、Pin Assign、工具栏/状态栏；保留尚未迁移 helper。
- [ ] WaveTheme 保留波形语义，适配背景文字；菜单/滚动条使用可读原生外观。
- [ ] 核实 SVG 资产与打包位置，改为 DPI 合适的 bitmap bundle；列出本轮未迁移的旧控件。
- [ ] 主题切换保持 selection、焦点、文本、tab 与布局，已关闭窗口不被回调访问。

完成条件：覆盖范围浅深色一致、中文可读、原生例外明确；MF-AC07/08 通过。

### GUI-11 视觉和可访问性验证｜必须｜集成｜依赖：GUI-07、GUI-10｜spec §6

- [ ] Windows 100/150/200% × 浅/深，1280×720 与 1920×1080；核对最小字号、按钮点击区域、焦点和对比。
- [ ] 主题切换、pane 浮动/找回各循环 20 次；副屏移除、最大化恢复、窄窗口均可操作。
- [ ] 禁用动画后功能一致；UI 样式/布局操作不启动业务任务，保存失败提示准确。

完成条件：截图矩阵及操作结果齐全；性能/闪烁问题有复现和处理，未测试环境明确标注。

### GUI-12 集成回归与签收｜必须｜集成｜依赖：GUI-11｜spec §6/7

- [ ] 按 BUILD 构建 sigflow 与相关测试 target，运行现有 Job/插件/Agent 等受影响回归并记录实际命令。
- [ ] 验证 edu/pro/dev、Agent ON/OFF 与 DeepSeek 插件存在/缺失，核对菜单/pane/布局过滤。
- [ ] 在原生 Linux 执行对应 GUI 场景并记录桌面/wx 版本；未执行不得写 PASS。
- [ ] 联合 Agent 面板注册演示，确认 theme/Window 菜单/布局扩展无重复实现。
- [ ] MF-AC01～09 逐项记录结果；回填本清单并更新 BUILD/用户布局说明中实际变更。

完成条件：MG4 通过；失败/未执行项明确影响，证据足以复现。

## 4. 后续项与跨规格依赖

- [ ] GUI-F01 单 notebook 页签拖出顶层浮窗：需独立 owner/parent/关闭恢复方案；本轮不承诺。
- [ ] GUI-F02 Auto-Hide：自绘侧缘栏与 pin 行为需另立规格；本轮 Close + Window 菜单满足收起找回。

| Agent task | 需要本清单的交付 |
|---|---|
| AG-01 面板骨架 | GUI-04 移除旧右栏；GUI-05/06 的 pane/菜单策略 |
| AG-02 主题交互 | GUI-08/09 共享 theme/art |
| AG-08 学习/工作区切换 | GUI-07 的 edition/mode 布局与可见性约束 |
| AG-13 集成验收 | GUI-12 的主框架回归与构建组合 |

Agent 可在独立宿主/Mock 中先开发组件，但接入 MainFrame 前满足对应依赖。两份 task 共同以 spec 的完成证据为准，不重复勾选对方尚未交付的基础任务。
