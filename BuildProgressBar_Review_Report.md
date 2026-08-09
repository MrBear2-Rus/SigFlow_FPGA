# BuildProgressBar 代码审查报告

**审查日期**：2026-08-09  
**审查范围**：`BuildProgressBar.h/.cpp`、`parse_progress.h/.cpp`、`MainFrame.cpp`（进度条相关部分）  
**审查人**：Claude Code（可维护性 / 鲁棒性 / 可读性视角）

---

## 一、审查总览

| 文件 | 新增行 | 发现问题 | 严重 | 中等 | 轻微 |
|------|--------|----------|------|------|------|
| BuildProgressBar.h | +82 | 1 | 0 | 0 | 1 |
| BuildProgressBar.cpp | +238 | 7 | 1 | 4 | 2 |
| parse_progress.h | +25 | 0 | 0 | 0 | 0 |
| parse_progress.cpp | +153 | 3 | 1 | 2 | 0 |
| MainFrame.cpp | +190 | 0 | 0 | 0 | 0 |
| **合计** | **+688** | **11** | **2** | **6** | **3** |

---

## 二、已修复问题清单

### 🔴 严重

#### P1: 取消后进度条立即消失，用户看不到"cancelled"状态

**文件**：`BuildProgressBar.cpp:230`（原）  
**现象**：用户点击 ✕ 后，`OnCancel` 立即调用 `m_onVisibility(false)` 隐藏 AUI pane，黄色"cancelled"背景闪烁一帧就不见了。  
**修复**：取消后延迟 2 秒自动隐藏，与成功态的 auto-hide 行为一致。`m_onVisibility` 调用移入 `OnTimer` 倒计时归零时，而非 `OnCancel` 中。

#### P2: nextpnr 正则大小写不敏感缺失 + 误匹配风险

**文件**：`parse_progress.cpp:34-38`（原）  
**现象**：
- 全部 nextpnr 正则无 `wxRE_ICASE` 标志。"Complete" 永远匹配不到小写 `complete`，Complete 阶段检测整体失效。
- `Plac(e|ing)` 会匹配 "**Replacing**"、"**Replacement**" 等非阶段输出。
- `Rout(e|ing)` 有冗余 `|Routing`（被前一个 alternation 覆盖）。

**修复**：
- 全部 nextpnr 正则加 `wxRE_ICASE`
- 加 `\b` 词边界锚定（`\bpack(ing)?\b`, `\brout(e|ing)\b` 等）
- 移除冗余 `|Routing`

---

### 🟡 中等

#### P3: 未使用的成员函数 `UpdateLayout()`

**文件**：`BuildProgressBar.h:57`, `BuildProgressBar.cpp:253-256`  
**现象**：声明且定义了 `UpdateLayout()`，但类内部无任何地方调用。  
**修复**：删除声明和定义。

#### P4: 假 fade 渐隐代码不工作且语义混淆

**文件**：`BuildProgressBar.cpp:191-194`（原）  
**现象**：
```cpp
unsigned char g = 235 + (10 * (15 - m_autoHideCountdown)) / 15;
SetBackgroundColour(wxColour(g, 250, g < 245 ? 235 : 250));
```
- `g` 范围 [235, 244]，三元表达式 `g < 245 ? 235 : 250` 的 B 分量恒为 235。
- 实际效果：颜色从 (235,250,235) 变到 (244,250,235)，肉眼不可辨。
- 倒计时本身就是够用的 UX 信号。  
**修复**：删除 fade 代码块，保留纯倒计时逻辑。

#### P5: 未使用变量 `elapsedMs`

**文件**：`BuildProgressBar.cpp:143`（原）  
**现象**：`long elapsedMs = m_stopWatch.Time();` 未使用。  
**修复**：删除该行。`FormatElapsed()` 内部自行读取 stopwatch。

#### P6: 无操作赋值 `SetValue(GetValue())`

**文件**：`BuildProgressBar.cpp:178`（原）  
**现象**：失败分支中 `m_gauge->SetValue(m_gauge->GetValue())` 没有任何效果。注释说"停在当前位置"，但 gauge 默认就停在当前位置。  
**修复**：删除该行。

#### P7: `BeginOperation` 无重入保护

**文件**：`BuildProgressBar.cpp:59`（原）  
**现象**：如果在 `m_running == true` 时再次调用 `BeginOperation`，旧 timer 仍在运行，两个 timer 并发会互相干扰。  
**修复**：`BeginOperation` 开头加 `if (m_timer.IsRunning()) { m_timer.Stop(); }`

---

### 🟢 轻微

#### P8: 确定模式下调用 `Pulse()` 引起闪烁

**文件**：`BuildProgressBar.cpp:77`（原）  
**现象**：`totalStages > 0` 时调用 `m_gauge->Pulse()` 将 gauge 切到 indeterminate 模式，紧接着 `AdvanceStage` 的 `SetValue()` 切回 determinate 模式——引发视觉闪烁。  
**修复**：删除 `Pulse()` 调用。确定模式下 gauge 直接从 0 开始。

#### P9: `GetParent()` 可能为 null

**文件**：`BuildProgressBar.cpp:91`（原）  
**现象**：`BeginOperation` 中 `Show()` + `m_onVisibility()` 后直接 `GetParent()->Layout()`，若 widget 已从父窗口分离则崩溃。  
**修复**：`if (GetParent()) GetParent()->Layout()`。

#### P10: 注释与代码不同步

**文件**：`BuildProgressBar.cpp:18`（原）  
**现象**：注释写"创建子控件（初始不显示，BeginOperation 时显示）"，但子控件从不独立隐藏——只隐藏整个 panel。  
**修复**：删除误导性注释。

#### P11: `OnCancel` 中 `m_cancelBtn` → `Hide()` 在下一次 `BeginOperation` 未恢复

**文件**：`BuildProgressBar.cpp:229`（原）  
**现象**：取消后 `m_cancelBtn->Hide()`，但 `BeginOperation` 未调用 `Show()`。  
**修复**：`BeginOperation` 中加 `m_cancelBtn->Show()`。

---

## 三、MainFrame.cpp 集成审查（无修复需做）

### Yosys Synthesis 路径
- `YosysExecutor::OutputCallback` 在 worker 线程中调用，`wxTheApp->CallAfter` 安全派发到主线程
- `progressLineBuf` 通过 `shared_ptr<wxString>` 跨线程共享，单 writer（单一 output thread）无竞争
- `wxWeakRef<MainFrame> frame` 防止 Frame 已销毁时崩溃
- ✅ 线程安全、生命周期安全

### nextpnr P&R 路径
- `FpgaToolProcess::LineCallback` 由 `wxTimer::OnOutputTimer`（主线程）驱动
- ✅ 全程主线程，无竞态

### Verilator 编译路径
- `SimulationEngine::ReportProgress` 在 `wxYield()` 循环中同步回调，全程主线程
- ✅ 无竞态

---

## 四、架构评价

### 优点
1. **松耦合做得干净**：`BuildProgressBar` 不知道 FPGA/EDA 概念，`parse_progress` 零 UI 依赖，`FpgaToolProcess` 只回调文本行。唯一胶水在 MainFrame。
2. **VisibilityCallback 解耦 AUI**：BuildProgressBar 不依赖 wxAUI，通过回调让宿主自行决定容器显隐方式。
3. **StageHint 结构体设计**：`stageIndex + totalStages` 让调用方同时拿到位置和预期规模，可做二次映射。

### 建议（不改代码，供后续参考）
1. `m_running` + `m_finished` 两个 bool 表示三个状态（Hidden/Running/Finished），后续可考虑改为 `enum class State { Idle, Running, Finished }` 单个枚举。
2. `FormatElapsed()` 返回 `wxString` 中 `wxT("%ldms")`，当毫秒数 > MAX_LONG 会溢出——实际不可能，Yosys 运行几小时早超时了，但从防御性编程角度可加 `static_cast<long>(ms)` 显式转换。
3. Verilator 编译的 3 阶段映射（`percent < 55` / `< 85`）目前是对 `ReportProgress` 内部百分比的硬编码假设，如果 SimulationEngine 调整百分比分段，进度条会显示错位。长期看可用回调传枚举代替百分比。

---

## 五、Changelog

| Commit 草案 | 说明 |
|-------------|------|
| `fix(progress): cancel shows state before auto-hide` | P1 |
| `fix(parse): case-insensitive nextpnr regex with word boundaries` | P2 |
| `chore(progress): remove dead code (UpdateLayout, fade, no-op, unused var)` | P3-P6 |
| `fix(progress): re-entry guard + null safety + cancel btn show` | P7, P9, P11 |
| `fix(progress): remove Pulse flicker in determinate mode` | P8 |
