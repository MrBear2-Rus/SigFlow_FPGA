> 负责人：js工
> 类型：规划
> 更新：2026-11-01
> 状态：进行中
> 阶段：U1

# js工 · 规划

## ① 当前目标

网页端从“单页控制台”改为与 wxWidgets 版一一对应的六页面应用：
产品落地页（介绍 + 登录注册）→ 工程管理（新建/打开）→ 工作区（六区 + 伪停靠）
→ 三个独立工具窗口。J0–J5 已完成的能力见 [history-js.md](history-js.md)，
架构与协议见 [README](../README.md) 与 [contracts](../backend/cpp/contracts/)。

## ② 页面清单（复刻基准）

| 页面 | 对应 wx 原版 | 打开方式 |
|---|---|---|
| `pages/landing.html` | 新增（原版无） | `/` |
| `pages/projects.html` | `ProjectStartWindow` | 登录后 |
| `pages/workspace.html` | `MainFrame` | 打开工程，`?project=<id>` |
| `pages/fpga-tool.html` | `FpgaToolWindow` | 新标签页，`?project=` |
| `pages/tracebridge.html` | `TraceBridgeWindow` | 新标签页，`?project=` |
| `pages/debug-contract.html` | `DebugContractConfigWindow` | 新标签页，`?project=` |

工作区六区：topBar（菜单 + 工具栏）、sideNav（3 个活动按钮）、left_sidebar（工程 /
SigFlow 树 / 元件库）、center（画布 + 编辑器）、right_sidebar（属性 / 引脚绑定）、
bottom_tabs（终端 / 波形）。
伪停靠：左 / 右 / 底三个投放区，中心固定；支持折叠、跨区拖动、同区换序、比例记忆。

## ③ 阶段与验收

| 阶段 | 内容 | 验收 |
|---|---|---|
| U1 | 浅色设计令牌、组件库（tabs / splitter / dialog / toast / empty）、图标迁移（ui_icons 13 + svg_icons 10 + svg 113） | 演示页视觉与原版一致，组件可交互 |
| U2 | P1 落地页 + 登录注册 | 注册→自动登录→进 P2；已登录访问 `/` 显示“进入工程”；失败有内联错误 |
| U3 | P2 工程管理页 | 列表含创建/修改时间；新建可选“空白/模板”；打开进 P3；删除二次确认；退出登录回 P1 |
| U4 | P3 工作区外壳 + 伪停靠 | 六区可拖拽/折叠；刷新后布局保持；未登录重定向 P1 |
| U5 | P3 面板填充 | 文件树、元件库、画布、编辑器、终端可用；属性与波形为明确空态 |
| U6 | P4–P6 独立窗口 | 新标签页打开并带 project；P4 任务可提交/取消；P5/P6 骨架完整 |
| U7 | 视觉校准 + 资源检查 | 对照原版截图逐块修正；静态资源 404 检查进 Node 测试 |
| 顺延 | J6 波形 / Agent | U7 之后按 C++ P7/P8 进度启动 |

## ④ 页面验收要点

**P1 落地页**

1. 顶栏 logo + 产品名，未登录显示 登录 / 注册；
2. Hero：标题 + 一句话定位 + 主 CTA；
3. 特性区三张卡片：可视化画布 / RTL 编辑与结构树 / FPGA 流程与波形；
4. 注册：userId + 密码（≥8）+ 确认密码，成功后自动登录并跳 P2；
5. 已登录访问 `/`：不显示表单，显示“进入工程”；
6. 窄屏单列可用；文案先占位。

**P2 工程管理**

1. 只显示当前用户工程，字段：名称、工程 ID、创建时间、最后修改时间；
2. 新建对话框：工程 ID（`[A-Za-z0-9._-]{1,64}`）、显示名（可选）、模板（空白 / 示例）；
3. 打开 → `workspace.html?project=<id>`；
4. 删除需二次确认，删除后列表刷新；
5. 空状态提示 + 新建 CTA；
6. 未登录访问 → 重定向 P1。

**P3 工作区**

1. 六区齐全，比例可拖拽，面板可折叠、可换区；
2. 菜单：File / Edit / Project / Simulate / FPGA / Help，未实现项置灰；
3. 工具栏：New / Open / Save / Reclaim / Start / Stop / Synthesize；
4. 未登录或工程不存在 → 回 P2。

**P4–P6 独立窗口**

1. `window.open(..., "_blank", "noopener")`，被拦截时回退同页；
2. 缺 `project` 参数或工程不存在时提示，并给回 P2 的链接；
3. P4 三页签：Yosys（任务列表 + 进度 + 报告）/ nextpnr / openFPGALoader。

## ⑤ 下一步

1. U1：`theme.css`（浅色调色板 + 间距 / 圆角 / 字号令牌）+ 组件库 + 136 个图标 + 演示页。
2. U2：P1 落地页（介绍文案先占位）。
3. U3：P2 工程管理，含后端补 `updatedAt` 与模板创建参数。
4. U4：工作区外壳与伪停靠。
5. U5：面板填充，元件库图标映射抽成 `assets/component-icons.json`。

## ⑥ 阻塞与依赖

- 结构树 / 诊断数据依赖 C++ P6（当前 501），P3 面板先做空态；
- 波形依赖 C++ P7，TraceBridge 依赖 P8；
- P2 需要后端补 `updatedAt` 与模板参数（空模板 / 示例模板）；
- 登录仍无 HTTPS / CSRF / 限流，上线前必须处理。

## ⑦ 相关决策

D-13（vendor 化）、D-14（构建入口）、D-15（画布存储）、D-16（history 文档），
见 [README](../README.md) 的「决策记录」。
