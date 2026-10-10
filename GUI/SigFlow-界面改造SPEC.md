# SigFlow 界面改造 SPEC（v1.0 · 规格基准）

> 配套：`app.html`（参考实现 / 交互实底）、`index.html`（模块 01–25 组件规格库）、`改造落地指南.md`（工程路线与 wx 映射）。
> 本 SPEC 是验收基准：任何落实现与本文冲突以本文为准；未声明项不得自由发挥。
> 目标工程 `D:\sigflow2026project\SigFlow_FPGA`（wxWidgets 3.2.9 + C++，MinGW GCC）。

---

## 0. 范围与红线

- 改造对象：整个主窗口视觉与交互层；**业务能力、菜单结构、快捷键语义、信息架构不变**。
- 红线：① token 全走 `UiTheme.h`，禁止硬编码色；② 无 `backdrop-filter`（真机近似）；③ 六栏（菜单栏/活动栏/主侧栏/辅助侧边栏/面板/状态栏）可显隐；④ 键盘可达 + 可见焦点；⑤ Terminal 插件诊断 `[Ready] eda-…` 可见。
- 视觉责任划分（优先级）：可读可对比 > 可用交互 > 信息层级 > 风格表达 > 装饰密度。

---

## 1. 全局 Token（`UiTheme.h` 对应 CSS 变量）

### 1.1 亮色（默认档）

| Token | 值 | 用途 |
| --- | --- | --- |
| `--hue-bg-1` | `#fbfbfd` | 窗体/页面底 |
| `--hue-bg-2` | `#f5f5f7` | 苹果灰主面 |
| `--hue-bg-3` | `#e8e8ed` | 更深一档 |
| `--surface` | `rgba(245,245,247,.66)` | 磨砂交互面 |
| `--surface-strong` | `rgba(255,255,255,.86)` | 面板/弹层本体 |
| `--surface-sunken` | `rgba(29,29,31,.045)` | 输入框凹陷底 |
| `--stroke` | `rgba(18,24,40,.10)` | 1px 描边 |
| `--stroke-strong` | `rgba(18,24,40,.18)` | 强描边 |
| `--text-1` | `#1d1d1f` | 正文墨 |
| `--text-2` | `#565a66` | 次级文字 |
| `--text-3` | `#878c99` | 辅助/占位 |
| `--accent` | `#0071e3` | 行动蓝：选中/焦点/链接 |
| `--brand` | `#1f8a4c` | 品牌绿：Synthesize CTA、画布放置描边、状态灯 |
| `--warn` | `#b25000` | Program 键、未保存 |
| `--danger` | `#c1121f` | 关闭键 hover |
| `--sel` | `rgba(0,113,227,.11)` | 选中底 |
| `--hover` | `rgba(16,24,45,.05)` | hover 淡底 |
| `--specular` | `rgba(255,255,255,.90)` | 结构面顶部 1px 高光 |

深色档：`--hue-bg-1/2/3 = #0a0b0e / #111216 / #050506`，`--accent #0a84ff`，`--brand #3ed37c`，描边反白 10%/18%。（P2 前逻辑锁浅色。）

### 1.2 尺度

- 圆角：`6 / 9 / 13 / 18 / 24px`（控件→卡片→窗口），胶囊 = min(w,h)/2。
- 间距：4 / 8 / 12 / 16 / 22 / 30px（8px 基线 + 微步）。
- 阴影：`--shadow-1 = 0 1px 2px rgba(16,18,27,.06), 0 6px 18px rgba(16,18,27,.07)`（普通键/浮层）；`--shadow-2 = 0 2px 3px …, 0 16px 44px rgba(16,18,27,.12)`（结构面/弹层）。**无立体玻璃层**。
- 动效：按压 140ms · 快态 180ms · 面板 260ms；`--ease-out cubic-bezier(.23,1,.32,1)`；只动 transform/opacity。

### 1.3 材质三条

1. 结构面 = `--surface-strong/sunken` + blur(28–44px) 近似 + `inset 0 1px 0 var(--specular)` + `--shadow-2`。
2. 按键 = 平面半透明或纯文字，无立体玻璃层。
3. 唯一例外 `Synthesize` CTA：液态玻璃七层（§3.3）。

---

## 2. 窗口骨架

| 区 | 尺寸 | 备注 |
| --- | --- | --- |
| 窗口 | `inset 18px`（外页留灰）圆角 24px；窄屏 <1024 充满 | `overflow:hidden` |
| 标题栏 | 高 38px | 无系统框，自绘；Windows 三键高 100% 宽 46px |
| 菜单栏 | 高 32px | 顶层纯文字高 24px |
| 工具栏 | 高 48px | chip 高 30px |
| 主工作区 | flex：rail 46 + 主侧栏 282(210–460) + 中区 flex + 辅助侧 264(210–520) | 间距 12px，内距 12px |
| 底部面板 | 高 174px（可拖/显示） | 对齐：左/右(固定 420–760)/居中(min(840px,88%))/两端(1fr) |
| 状态栏 | 高 ~26px（padding 7+7 + 10.5px 文） | 可整条隐藏 |

页面底 = `linear-gradient(160deg, bg-1, bg-2 55%, bg-3)`，无彩色壁纸。

---

## 3. 组件规格

### 3.1 标题栏
- 左→右：品牌（19px 绿渐变方牌 + 12px 白波形）+ 13px/650 `SigFlow`；路径分组 `26_9_29_test · src / top.v`，13px，`b` 600；`● 未保存` 11px `--warn`、tabular-nums。
- 右端：布局簇（4 个键 28×26、6px 圆角、间距 1px，aria-pressed 灰 3 色）→ win-controls（hover 灰淡底；close hover `--danger` 底白字；图标 11px stroke 1.4）。

### 3.2 菜单栏
- 顶层项：12.5px `--text-2`，padding 0 10px，高 24px，圆角 6px；**hover 仅文字变 `--text-1`，无底色**；**展开（选中）时 `--sel` 底 + `--accent` 字**；点空白收起即取消。
- 浮层：宽 ≥232px，padding 6，圆角 13px，`--surface-strong` + blur(28px) saturate(180%) + 描边 + `--shadow-2` + 顶部高光；进入 rise 180ms。
- 项：12.5px，padding 6 9，圆角 9px；hover `--sel` 底 + 蓝；禁用 45% 透明、hover 无底；快捷键右对齐 11px tabular-nums；分隔线 `--stroke`，边距 5 6。

### 3.3 工具栏 chip

| 变体 | 底 | 描边 | 字 |
| --- | --- | --- | --- |
| 默认 | `--surface` | `--stroke` | `--text-1` 12.5px/510 |
| hover | `--surface-strong` | — | — |
| primary(Synthesize) | **液态玻璃七层** | `1px double rgba(51,51,51,.08)` | `--text-1`；`⌘R` 11px `--text-3` |
| warn(Program) | `--surface` | `--warn` 34% | `--warn` |
| steady(PnR 状态) | `--surface` | default | `--text-2`，cursor default |

按压 scale(.97) 140ms；图标 14px。

**Synthesize 液态玻璃七层（亮色 token）**：
1. 外投影 `0 4px 8px rgba(0,0,0,.20)`
2. 暗内环 `inset 0 0 2px rgba(0,0,0,.80)`
3. 白内影 右上/左下 `inset ±2px 1px -1px rgba(255,255,255,.90)` 压在暗环上
4. 次级白内影 `inset ±6px 1px -6px rgba(255,255,255,.55)`
5. double 弱描边 1px
6. `::after` 内缩 4.5px × 1px 白环 blur(1px)
7. `::before` 中心 35% 内缩 16px 暗环 blur(8px)（真机可跳过，加深第 2 层补偿）
hover：本体渐变不变，外投影升为 `0 8px 20px rgba(0,0,0,.20)`；focus：蓝 3px 环。

### 3.4 图标按钮（tbtn）
30×28 圆角 6px，透明底 `--text-2` 图标 16px stroke 1.7–1.9；hover `--hover` 底 + 字色深；pressed scale(.94)；selected（主题键/工具）`--sel` 底 + `--accent` + 描边。

### 3.5 活动栏 + 主侧栏
- 活动 46px；键 34×34 圆角 9px；选中 `--sel` + `--accent` + 左指示条 3×18 圆角 2px。
- 树：字号 11.5px；分组头 9.5px/600 `.07em` 大写 + 前导 10px 横线；节点 padding 6×8；hover `--hover`；选中 `--sel` 底 `--accent` 字 + 图标同色；徽章 9.5px 胶囊描边 tabular-nums。

### 3.6 中区（画布 62% / 编辑器 38%，拖 25–80%）
- 画布：白底；网格 20px/100px；导线 2px 圆角折角，结点 3.6px；元件卡 12px 圆角 1px 描边+软影；选中蓝 1.6px + 8 手柄 7×7 圆 1.5px；幽灵（品牌绿虚线 6 5 / 62%）；shadow filter dy1.6 σ2.2 16%。
- 浮动工具条（顶部中央）/ 缩放 HUD（左下）：磨砂胶囊（`--surface`+blur+描边），内 tbtn 30×28，`z` 数字 tabular-nums。
- 画布右上「拖出」键：gd 玻璃胶囊小圆钮，图标 1.7 stroke。
- 编辑器头：15px 文案 + `Tree-sitter · 已校验` 徽章右端 + 拖出键；代码 12.5px SF Mono；字色 kw 蓝 / ty 绿 / st 橙 / cm 斜体灰；gutter 右对齐凹陷底。

### 3.7 辅助侧边栏（永远最右，默认 264px）
- 三页（Property / Pin Binding / DeepSeek）；表单 label 11px/500 `--text-3` + 输入 11.5px 高 28px（凹陷底 1px 描边，focus 蓝描边 + 3px 蓝环 22%）；select 自绘下拉箭头；只读项色弱。
- Agent Chat 气泡同 node 规格。

### 3.8 底栏 + 状态栏
- Terminal 11.5px/1.7 SF Mono：`$`+命令 蓝；`[Ready] eda-… 1.0` 品牌绿；`[WARN]` 黄黄橙类；深底（term 局部黑底 #0d0d0f 设计，浅色下保留深底贴近 IDE 观感）。
- Waveform：行高 32px + 34px svg；信号名 10.5px mono 右对齐；clk 绿方波 / rst 蓝 / bus 橙分组步进 / y 墨色 82%。
- 状态栏 10.5px `--text-2` tabular-nums：左 = 状态灯(7px + 3px 环) + 当前操作；右 = 坐标 / Grid 20 / Zoom / `dev · gcc-16.2.0`。

---

## 4. 自定义布局系统

### 4.1 状态
```json
{ menubar:true, rail:true, left:true, right:true, bottom:true, statusbar:true,
  side:"left|right", bal:"left|right|center|just", pos:"bottom|top" }
```
持久化：localStorage `sigflow-layout-v2` ↔ 工程侧 wxFileConfig `sigflow-layout`。

### 4.2 Quick Pick 面板（340px）
- 顶部标题行 + Esc 关闭；
- 预设 chips：经典 / 编码优先 / 画布优先 / Chat 分离 —— 与状态实时比对，全命中打亮；
- 可见性组 6 行（图标+名称+右侧 kbd）：菜单栏 `Ctrl+Shift+M` · 活动栏 `Ctrl+Shift+A` · 主侧栏 `Ctrl+B` · 辅助侧边栏 `Ctrl+Alt+B` · 面板 `Ctrl+J` · 状态栏；隐藏项整行转灰；
- 主侧栏位置：左 ✅ / 右（**辅助侧边栏恒最右**）；
- 面板对齐：左 / 右 / 居中 / 两端（右侧屏固定 420–760px，居中 min(840px,88%)）；
- 面板位置：底部 ✅ / 顶部对齐（order 交换 bottomwrap ↔ workspace）；
- 工作区拖出组：绘图 / 代码 / Chat → 独立窗口。
- 行 28px，hover `--hover`；单选组当前项 `--surface` 底 + `--accent` ✓；浮层 Rise 220ms；焦点环 4px 蓝。全部行键盘可达。

### 4.3 快捷键映射
Ctrl+B / Ctrl+Alt+B / Ctrl+J / Ctrl+Shift+M / Ctrl+Shift+A → 显隐翻转 + 状态栏反馈；Esc 关面板/菜单。

### 4.4 独立窗口
- 入口：画布右上钮 / 编辑器头右上钮 / Chat 页签栏右钮 / Quick Pick 拖出组。
- 弹出：`880×620`（工程侧 = `wxFrame` 或 AUI 浮动 pane），按 `wxDisplay::FromPoint(mouse)` 落屏，可手动拖去任何显示器。
- 主窗占位：区显示 `已拖出为独立窗口 —— 可拖到另一块显示器` + `回归主窗口` 钮；回归 = 关闭子窗 + 还原区。
- 子窗头：34px（`SigFlow · 主视窗名（独立窗口）` + `回到主窗口` 钮）；子窗 = 单工作区视图（画布缩放/代码只读演示账可用），隐藏主壳 chrome。

### 4.5 主侧栏右对齐语义
order 交换：主侧栏移到画布右侧邻位（order2 ↔3 中区在前，辅助侧外缘最右恒定）；此态下左右栏拖拽把手隐藏（保证辅助侧最右不受牵引）。

---

## 5. 无障碍与降级

- 焦点：所有可交互件可见焦点环（蓝 3–4px / 45% 透明）；顺序按 DOM。
- 对比：正文/底 ≥4.5:1（`#1d1d1f` on `#f5f5f7` ≈ 13:1）；辅助灰 ≥3:1（非正文）；选中/状态不裸靠 color（图标/文字/形状并列佐证）。
- `prefers-reduced-motion`：动画 dur→0.x，位移动画关闭。
- `prefers-reduced-transparency`：结构面/浮层/chip blur 全关、底换实色。
- `prefers-contrast:more`：面换 `--canvas` 实底 + 描边 `--text-1`；选中节点额外 outline。

---

## 6. 验收清单（逐条）

| # | 项目 | 判据 |
| --- | --- | --- |
| V1 | 六栏显隐 | Quick Pick + Ctrl+B/Alt+B/J/Shift+M/Shift+A 全部生效且即显即隐 |
| V2 | 显隐语义正确 | 隐藏后无空洞/无滚动条出 Graphing；状态栏反馈当前变化 |
| V3 | 主侧栏右对齐 | 主侧栏贴中区右缘；辅助侧依旧屏外缘 |
| V4 | 面板 4 位对齐 | 底(居中760px) / 顶(项目区覆盖主工作区，无遮 iframe) 均无溢出 |
| V5 | 独立窗口 | 三区弹出均可跨显示器；回归恢复原位态 |
| V6 | 字体层级 | 树/表单/状态栏比画布/代码/CTA 小 ≥1px 且轻一档 |
| V7 | Synthesize 玻璃 | 七层完整；hover 提影不变底；focus 蓝环可 focus |
| V8 | 菜单栏 | 平时无任何按键设计；打开时高亮；点空白收起即取消 |
| V9 | 主题 | 浅/深两档可切 + 跟随系统；两套 token 独立取值，不互混 |
| V10 | 键盘 | Tab 顺可走，focus 环可见；Esc 一口收全 |
| V11 | 窄屏 | <1024 主/侧栏 clamp；<760 辅助侧隐藏；无横向滚动 |
| V12 | 快捷键/命令 | 原工程快捷键与命令语义全保留（含 F6 仿真等） |

---

## 7. 版本

- SPEC v1.0（随 app.html 当前态）—— 定格于 Apple 亮色规范 + 自定义布局 + 独立窗口。
- 变更流程：修改任何规格需同步改：`app.html`（参考实现）+ `index.html`（模块图）+ 本 SPEC + `改造落地指南.md`（若涉工程方案）。
