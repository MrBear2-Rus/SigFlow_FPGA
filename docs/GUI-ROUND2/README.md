# SigFlow GUI 第二轮：区域圆角与视觉统一

> 2026-10-12 · v1.0 · **实现已落地，等待 Windows 视觉/交互签收**。依据用户本轮提供的两张图片及当前源码；Windows 为本轮交付平台，Linux 不验证。

本轮把当前连成一片的直角停靠区，整理为有留白、圆角和轻量层次的工作区，并统一页签、列表、表单、图标与终端。延续 DeepSeek 已完成的主题、键盘、布局和子窗口适配，不重新实施第一轮。

## 1. 文件与阅读顺序

| 文件 | 用途 |
| --- | --- |
| [spec.md](spec.md) | 视觉规范、范围裁决、技术路线与保护边界 |
| [gap-analysis.md](gap-analysis.md) | 两图差距及源码落点，区分观察与待验证事项 |
| [task.md](task.md) | RV-T00～RV-T08 共 9 项任务、依赖、完成条件与回退 |
| [traceability.md](traceability.md) | 规范 → 任务 → 代码 → 验收的对应关系 |
| [validation.md](validation.md) | 固定场景、截图与量测方法、功能回归和签收条件 |
| [acceptance.md](acceptance.md) | 本轮实施证据与缺陷台账；代码/构建证据已记录，真实窗口 PASS 仍待复核 |
| [_reference/expected.png](_reference/expected.png) | 用户第一张图：视觉目标 |
| [_reference/current.png](_reference/current.png) | 用户第二张图：现状参考；不能代替可复现程序基线 |
| [_reference/manifest.json](_reference/manifest.json) | 附件来源、原始尺寸与 SHA-256 |

## 2. 与已有规划的关系

第一轮最新事实来自 [acceptance.md §28](../GUI-CHUGAO/acceptance.md#28-t-140-落地--深色档开启--v6v8v9v10-全部收口2026-10-12--deepseek-接续)，而不是旧 task 的未勾选状态。新任务采用 `RV-` 前缀，避免覆盖 T-xxx 记录。`GUI/` 仍是冻结设计输入。

新图与旧规格不同的项目，在本轮 [spec §2](spec.md#2-本轮范围与冲突处理) 明确覆盖：页签绿色指示线、Terminal 随主题、工作区卡片底色和间距。其它既有行为继续有效，包括按钮只在 Hover/按压出现蓝边、画布元件透明且无容器边框、布局持久化和现有命令入口。

## 3. 交付顺序

RV-T00 基线 → RV-T02 单区域原型 → RV-T01 token 补齐 → RV-T03 全区域接入 → RV-T04 页签 → RV-T05 列表/图标 → RV-T06 表单 → RV-T07 终端 → RV-T08 集成签收。代码已经按该顺序落地；当前只剩 Windows 前台截图、交互和量测证据，**先验证圆角容器与原生子控件能共存，再完成签收**。

本轮不会顺带增加图一的 Place & Route / Program 等工具栏按钮，不改业务和信息架构，不定制操作系统标题栏。预期效果图中的悬浮球不是 SigFlow 组件，不纳入计划。
