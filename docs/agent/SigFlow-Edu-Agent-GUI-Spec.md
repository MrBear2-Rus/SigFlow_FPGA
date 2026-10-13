# SigFlow Edu Agent GUI 规格

> 版本：v1.1 · 2026-10-09。状态：实施规格，未实现、未验收。  
> 来源：相邻 `SigFlow_FPGA/docs/agent/SigFlow-Edu-Agent-GUI-Spec.md` v1.0；本版按当前 CMake 仓库、实际 sidecar 契约和 10.8 接口方案校订。  
> 执行清单：[task.md](task.md)。共享 UI 基础：[主窗口 spec](../gui/SigFlow-MainFrame-Upgrade-Spec.md)。  
> 接口依据：[10.8 接口调整方案](../edu-agent/sigflow/10.8-Agent接口调整方案.md)、[SigFlow spec](../edu-agent/sigflow/spec.md)、[Agent spec](../edu-agent/ucagent/spec.md)、`contracts/edu-agent/v1/`。

## 1. 产品目标和交付边界

保留原稿的浅色纸面、克制语义色、教学引导侧栏、助手卡片、四视图联动、概念卡、波形标注、教学调试和回退时间线。以新增 wxAUI DockPanel 和浮层接入主窗口，复用图码、Job、证据、波形及 TraceBridge 能力。

本版将“界面展示目标”“现有服务可接入功能”“需要后端扩展的教学闭环”分开验收。UI 不自行生成教学状态、参考答案权限或验证成功结论。

| 阶段 | 可交付内容 | 启用条件 |
|---|---|---|
| A：只读教学接入 | 面板、异步会话/请求、解释/提示/报告卡、只读计划建议、证据定位、状态降级 | 实际 v1 契约验证和宿主安全接通；具体卡片按能力显示 |
| B：教学闭环 | 服务端提示升级、L4 receipt、阶段决策/审批执行、课程/记录、可信进度与恢复 | 各扩展契约、数据生产方与对应后端 Gate 已完成 |
| C：扩展教学体验 | 完整词典、实板教学调试、工作副本恢复时间线 | 各自真实服务/设备与验收具备；可分批交付 |

阶段 C 仍属于完整目标，但不阻塞 A。A 不能被写成完整教育版完成。界面可使用带显著“演示数据”标签的 Mock 开发；生产环境不得以 Mock 填充未实现的能力。

### 1.1 冲突处理和职责

- 冻结契约和已确认安全边界优先于界面草图；新增动作先同步 schema/OpenAPI/fixture/生产方/消费方。本文不覆盖旧 spec 的权限要求。
- C++ 宿主负责可信用户交互、工程事实、授权和 Job；Python Agent 负责教学策略、卡片和学习记录。UI 不在本地伪造后端教学状态。
- Agent 不运行任意 shell、不改学生 RTL、不开放教育 Agent 的 pnr/pack/flash/debug。只读解释/提示是 Agent 请求，不要求伪装成 EDA Job；真实工具操作走现有 Job 和授权通道。
- 主窗口 spec 负责 UiTheme、pane/Window 菜单和布局存储。本规格只增加教育 pane、适配器、控制器和教学交互。

## 2. 代码基线与组件落点

本节只表示静态核对到的资产；不构成实现完成报告。建议新增名称可在实施时调整，但必须回填 task。

| 资产/组件 | 位置 | 使用方式 |
|---|---|---|
| 宿主生命周期 | `main/agent/AgentServiceController.*`、MainFrame 的 Gateway/sidecar 初始化 | 复用启动/health/代次，不新建第二个 sidecar |
| UI → Agent | `contracts/edu-agent/v1/agent.openapi.yaml`、`agent/src/sigflow_edu_agent/` | 新增 typed client/会话控制器，解析同步 201 run |
| Gateway/证据 | `core/src/eda-agent-gateway/` | 复用 snapshot/context/report/wave 与授权存储 |
| 当前中央区 | `MainFrame.cpp` 的 canvas/RTL `mainSplitter` | 通过显式 view 导航适配，不能假定已是四 tab |
| 当前波形 | `bottom_tabs` 的 Waveform、`main/wave/WaveformView.*`、`main/trace/` | 复用唯一波形实例，查询/标注分层 |
| 代码定位 | MainFrame 的 `m_verilogEditor->GotoLine`、图码映射 | 增加受控 SourceRef 导航桥；未找到名为 JumpToRTLLine 的公共接口 |
| 调试 | `main/debug/TraceBridgeWindow.*`、`main/debug/` 的会话服务 | 核查已有导出/取消/对比接口，原稿 ExportTarGz 名称未验证 |
| 快照 | `core/src/eda-agent-gateway/SnapshotService.*` | 现有不可变证据快照不是工程恢复 API |
| 拟新增 | `main/agent/AgentClient.*`、`AgentSessionController.*`、`EduGuidePanel.*`、`AgentAssistantPanel.*` | 网络/状态与渲染隔离，MainFrame 只装配与路由 |
| 拟新增适配 | `AgentContextBridge.*`、`AgentNavigationBridge.*`、`TeachingStateAdapter.*` | 主线程复制 DTO，后台只读，不共享控件/树裸指针 |

所有新增 C++ 文件进入根 CMake 显式源列表；测试进入相应 tests/agent 或 tests/contract 构建入口。`SIGFLOW_BUILD_EDU_AGENT` 开关关闭时不引入 Agent 运行依赖。

## 3. 共享视觉和可访问性

使用 [主窗口 spec §5](../gui/SigFlow-MainFrame-Upgrade-Spec.md#5-p3统一主题字体与-dpi) 的唯一 UiTheme palette、system/light/dark 偏好及 DPI/字体规范。教学面板使用 guide/done/hardware/error 的背景/文字对，间距 8/12/16 DIP，圆角卡片 10 DIP、按钮 6–8 DIP。

- 两档字重 regular/medium，标题 14 DIP、正文 12、辅助 11、最小 10；单位转换遵守 UiTheme。
- 1 DIP 发丝线，不用投影/渐变/发光。原生窗口标题栏保留平台行为，不绘制假的 macOS 窗口按钮。
- 状态用文字/图标和颜色共同表达；文字可复制。所有按钮/阶梯/概念卡支持 Tab、Enter/Space；Esc 关闭浮层并返回触发对象焦点。
- hover 不是唯一入口。概念卡有可聚焦的点击按钮，浮层在屏幕边缘改向并限制于工作区。
- 新组件可无动画交付；有动画时 150–200ms、局部重绘，减少动画/关闭窗口时停止 timer。

## 4. 布局和模式

| 组件 | AUI 名称 | 位置/行为 |
|---|---|---|
| 教学引导 | `edu_guide_sidebar` | 左侧，独立可停靠工具 pane；不替换原项目/属性栏 |
| 教学助手 | `agent_assistant` | 右侧新增工具 pane，独立于已删除的 right_sidebar |
| 教学流水线 | `edu_pipeline` | 底部，避免与 bottom_tabs 强制重叠 |
| 中央工作区 | `center_area` | 保持主窗口 canvas/RTL 宿主与底部波形的既有结构 |
| 模式选择 | 并入 topBar | “学习 / 工作区”，只改变教育 pane 和导航密度 |

A 阶段不能把“工作区”文案写成已完成的“工程 Agent”。`SIGFLOW_EDITION=edu` 且功能开关启用时才提供学习入口；pro/dev 仅按明确配置提供，不因拖入旧 perspective 自动启用。

初始助手宽建议 320 DIP、教学侧栏 240 DIP、流水线 64 DIP；中央可用宽优先。窄窗口采用单侧栏显示/手动找回，指南与项目栏不要同时占满屏幕；具体默认由 GUI 与教学面板联合截图验收。所有工具 pane 使用主窗口的停靠/菜单/布局策略。

模式切换目标为主线程可见性更新 <500ms，不触发模型、EDA、工程重载或业务状态清空；请求进行中只改可见性，不自动取消 Job。按主窗口 `edition/mode` 的布局键保存；主题是应用级，不写额外 `.sigflow/ui/mode.json`。功能不可用/工程只读时仍提供稳定说明和原有编辑入口。

## 5. 状态和接口约定

### 5.1 会话与请求

业务会话绑定 `(IDE instance, sidecar generation, project_id, revision, session_id)`。请求另外携带本地 request ID 与选择内容 hash；run/card 的 UI 键为该绑定加 run/card ID，不能只用 `card_id`。

| UI 状态 | 显示与允许操作 |
|---|---|
| 未启用/未安装 | 安装/配置说明；不循环请求 |
| 启动/重连中 | 状态和取消当前等待入口；提交暂禁用 |
| 无工程 | 提示 New/Open；课程静态帮助可用 |
| ready/会话就绪 | 按真实 capabilities 提供请求 |
| 请求中 | 单会话串行，显示等待；可取消本地等待，不宣称服务器 run 已取消 |
| degraded | 明示规则模式及原因；仍可读有效卡片 |
| failed/断线 | 原因与显式重试，不把失败卡当成功结果 |
| 历史/上下文失效 | 保留可读历史标签，禁用旧卡执行、升级和当前证据定位 |

- 当前 A 路由支持 `explain/hint/plan/report_review`，同步返回 HTTP 201；UI 在后台调用保持主线程响应，不等待未实现的异步状态路由。
- 同会话请求串行化；解析实际 schema 的 required/union/枚举/长度，未知种类显示 unsupported，不能按长相执行。
- 当前 Agent events 立即返回，无 `wait_ms`；采用有界轮询/退避、去重与分页补读。Gateway events 的长轮询策略单独处理。游标只前进到实际消费的序号，不直接跳到 watermark；过期/丢页明确提示和重新同步。
- 用户取消等待或超时仅停止本地展示/后续请求，迟到响应按上下文丢弃；只有服务提供取消能力时才显示“已取消服务运行”。有副作用的审批/Job 不盲目重试。
- 切工程、保存形成新 revision、sidecar 重启/轮换 token 时，旧飞行请求失效；旧会话内存不能跨重启恢复，重新建会话并提示。dirty 编辑可解释，但明确“未保存缓冲”；旧磁盘报告不能冒充当前缓冲结果。
- 超时/网络错误/401/404/410/版本不符的 UI 路径和 retryable 判断由协议适配层统一；UI 展示去敏的 code/request_id，token 与绝对路径不进入界面或日志。

### 5.2 可信内容与证据

TeachingCard / PlanCard 为 schema 定义的 union；PlanCard 不依赖不存在的 kind 字段判断。卡片文本当不可信数据：使用文本/受限富文本，禁止脚本、任意文件 URL、shell/外部协议链接。来源只用 opaque ID，由宿主转换成受控工程定位。

报告必须核对 project/revision/job/origin 与完整性。历史、legacy、partial、失败报告可解释但不能写成“当前验证通过”。无映射/歧义 SourceRef 返回 unavailable 并说明，不能猜 RTL 行。无真实模型时显示 `model_used=false` 的规则降级，不伪装模型回答。

## 6. 组件规格（保留原 T 编号）

### T-1 学习/工作区切换

胶囊式互斥控件，激活项强调色/背景和清晰焦点，点击与键盘均可操作。只切 pane 可见性与视图导航；保存布局前后均保持工程、Job、会话历史。没有学习功能时隐藏选择器或禁用学习项并说明，不承诺工程自动化。

### T-2 教学引导侧栏

六步展示：`原理图设计 → 图码同步 → 仿真验证 → 综合与资源 → 教学调试 → 练习小结`。这是展示分组，不等同旧 spec 的 E0–E7 后端状态；B 阶段先冻结 E 状态到六步的映射和完成证据。

圆点完成/进行/待办，异常额外显示阻塞/需复测文字。完成态来源为服务端教学记录及绑定当前 revision 的证据，不来自用户点击或“卡片说通过”。修改工程后依赖旧 revision 的步骤显示需复测。

A 阶段可显示操作导航与静态步骤说明，不填虚构 3/6 进度；课程包 unavailable 时显示能力说明，不显示假课程进度。点击已完成步骤只导航/查看历史，不自动恢复快照或重跑工具。每步讲解 2–3 行，支持长文字换行。

### T-3 四视图联动

一期在现有布局上提供四个导航入口“原理图 / RTL / 波形 / 资源”，先复用 canvas/RTL splitter 与底部波形；资源为新增只读报告面板或现有报告入口。强制改成中央四 tab 需另行评估重挂/布局迁移，不作为 A 阶段隐含要求。

- canvas/tree 选中 → 通过可信图码映射定位 SourceRef；波形信号 → 同一导航桥联动画布/RTL。加来源标识与去重，避免互相发 selection 造成循环。
- 无映射、删除对象、历史版本时说明不可定位；跨工程引用拒绝。
- LUT/FF/IO 显示实际统计与目标容量；分母缺失用数值/未知容量，不能凭空画百分比。阈值来自目标 profile，缺失则不显示“超标”结论。
- 导航不复制业务 panel，不更改报告事实；当前 workspace 的波形与助手上下文一致。

### T-4 概念卡浮层

hover 600ms 或键盘/点击触发；离开锚点和浮层 300ms 后关闭。浮层标题（中英术语/分类）、定义、可选图示、关联概念。未知词显示简短“暂无词条”或不给 hover 入口；缺图示只隐藏图示区。

词典为随包只读资源，记录 schema/version、稳定 concept ID、内容来源及教师审阅；可选用户缓存放应用数据目录，不要求工程里存在 `.sigflow/agent/concepts/dictionary.json`。至少 50 个审阅词条作为 C 阶段目标，A 阶段不借此宣称课程检索可用。概念点击历史有限制，非法关联 ID/循环不无限递归。

### T-5 助手、分级提示和决策卡

头部“教学助手 / 引导式讲解”，带服务/规则模式状态；对话区学生右对齐，助手左对齐。底部输入/发送/停止等待；输入预算遵循接口，超限前给清晰提示，不截断后悄悄发送。长会话分页/限制已挂载控件数，保留滚动锚点，用户查看旧消息时新回答不强制滚底。

| 提示级别 | 展示目标 | 启用要求 |
|---|---|---|
| L1 | 方向性提问 | 实际能力允许 |
| L2 | 现象/机制提示 | 用户主动请求；A 中明确是选择提示级别 |
| L3 | 模块/信号定位 | 用户主动请求及有效证据映射 |
| L4 | 经策略允许的参考解要点 | B 的真实两次独立操作 + 可信 receipt；A 失败关闭 |

严格 L1→L2→L3 递进由 B 的教学服务决定并拒绝跳级；不能只在 UI 禁按钮后宣称服务端约束已完成。A 的固定级别请求不标记成持久化学习阶梯。

L4：第一次“申请查看参考解”产生绑定工程/版本/动作/策略的 challenge；第二次独立确认显示影响说明并由可信 C++ UI 签发 receipt；服务核验并原子核销后返回。只写 `more=true`、在 UI 做确认框或记录本地布尔值均不足以解锁。未知/不支持能力、旧卡、过期 challenge/receipt、跨工程/版本全部关闭入口；当前 v1 capabilities 不可用时即便服务可被旁路也不发送 L4 请求。

决策卡展示证据、当前状态和 `继续 | 改参数 | 返回查看 | 中止`，A 阶段只做导航/计划建议，无执行按钮。B 中：

- “继续执行”明确列出 plan、参数、snapshot/revision、能力及配额，由可信宿主签 grant；计划变化或过期需重新审批。
- 改参数生成新计划并重新审批，不能修改已签计划后沿用 grant。
- “返回查看”仅历史导航；实际恢复须走 T-8 的独立确认流程。
- “中止”有 Job 时走现有取消并等待真实终态；没有取消能力时说明仅停止后续步骤，不伪造已终止。
- Agent 保存学习决策，SigFlow 保存可信审批/执行审计；二者通过 action ID 关联，不共写同一数据库、不把学习记录写成权限证明。

错误根因卡三段“发生了什么 / 可能原因 / 修复方向”，同时显示证据及不确定性；关联概念可打开 T-4。未知卡片只显示类型与不可用说明。计划卡默认只读，合法依赖、能力与授权 Gate 完成前不进入执行链。

### T-6 波形标注与提问

在现有波形展示层增加只读标注 overlay，不另建波形解析器或修改 trace 查询语义。标注来自实际分析/对比服务，绑定 artifact/project/revision、signal ID、timebase 与时间范围。

- 复位释放：绿色虚线；触发：蓝线；首差异：红线与目标信号行背景。未知/不支持算法时不生成标注。
- overlay 使用同一 time→pixel/scroll/zoom/行坐标转换，裁剪于 viewport；大时间值不经浮点或 32 位整型丢精度。
- 首差异带实际差值与单位，有有效 SourceRef 才可双击定位。没有明确对齐基准时不给“晚 2 拍”的结论。
- 框选范围后可点击“向助手提问”，上下文为受控的 signal IDs、十进制字符串 tick 范围、timebase、artifact/revision；不传绝对路径或整个 VCD。截断/采样/信号上限明示，保留 x/z。
- 当前 run schema 不支持的波形动作不能随意塞入 v1 请求；先冻结 context 扩展、schema/fixture 和生产方，再启用提问按钮。A 阶段可只查看/定位标注。
- 打开新波形、换 revision 或切工程时撤掉旧 overlay；不挡拖动、缩放、原有选择和快捷键。

### T-7 教学调试三步

展示 `选信号 → 采集 → 对比讲解`，内部复用原 TraceBridge 多阶段状态，不能删除必要的编译、约束验证、设备准备与失败状态。

这是学生手动操作的独立教学调试扩展，不向教育 Agent 开放 debug/flash。课程允许、设备/已兼容 bitstream/信号映射/权限齐备时才启用；默认课程仿真闭环，不提供无人确认烧录。

- 串口友好名可推荐，但连接/采集前显示并确认实际设备，不只因匹配 Sipeed/Tang 字符串就自动控制设备。
- CST/契约基础字段可只读；高级项隐藏前要有经验证的课程配置，缺失时报告准备不足，不绕过验证。
- 采集可取消，映射底层 cancellation 并等待清理，超时/断线不显示成功。sim/hw 蓝橙双轨，playhead/zoom 对齐；对齐不充分明确说明。
- 讲解依赖真实对比证据，规则/模型不可用时仍可查看原结果；不以生成讲解代替采集通过。
- 失败导出复用已有能力；实施先确认实际 API。导出需说明内容与目标路径，去掉 token/敏感路径，不假定未验证的 `DebugSession::ExportTarGz` 已存在。

### T-8 流水线、历史和回退

流水线 `仿真 → 综合 → 资源 → 调试` 是展示摘要，与 T-2 六步共用 TeachingStateAdapter；完成/当前/待办/需复测来自可信状态。A 阶段可显示真实 Job/报告状态与历史，不承诺完整课程完成。

当前 SnapshotService 为不可变执行证据，默认路径为 `.sigflow/agent/snapshots`；原稿 `.sigflow/snapshots` 的 copy-on-write 和任意恢复尚未实现。禁止把查快照等同可写恢复。

先提供“查看此步证据/查看历史”入口。C 阶段实际恢复需新增受控服务：

1. 用户选择历史对象，展示文件清单、版本和将恢复的范围；dirty 时先选择保存/保留到新工作副本/取消。
2. 校验项目归属、文件 hash/路径白名单、快照完整性和当前是否有使用该输入的任务；不恢复工程外任意路径、凭据、学习 DB 或旧 grant。
3. 默认创建独立可编辑工作副本，原工程保留；如支持替换当前工程则须显式确认、先备份并采用事务/失败恢复。
4. 恢复后重建图码、工程 revision/会话，撤销旧审批并将后续验证标需复测；历史报告仍是历史证据。

学生可手工编辑工作工程；Agent 不自动写码。不可变证据快照、工作副本和工程恢复备份分别管理，不能用“所有修改都在快照副本上”掩盖现有编辑器行为。

## 7. 授权、隐私和线程

- UI→Agent token、Agent→Gateway token、Gateway UI token 三类身份分离；后者不传 Python/卡片。界面代码通过可信控制器调用，不持有可从富文本执行的 grant/receipt 回调。
- 课程/画像/学习记录能力上线前明确存储方、用户告知、导出/删除及保留策略；A 内存对话不得标成跨重启学习记录。
- 主线程访问 wx 控件/树/图元，复制 immutable DTO；后台负责 HTTP/解析/耗时查询。回调使用弱引用、generation/context 校验，关闭窗口/宿主时解绑与回收，不访问已销毁控件。
- 性能目标：导航/模式可见性切换 <500ms；网络期间界面可拖动/编辑；大 VCD 仅传查询范围。部署平台沿用当前 BUILD 与 Agent 发布要求，不用 WSL 代替原生 Linux 证明。

## 8. 验收矩阵

| 编号 | 场景 | 验收标准/阶段 |
|---|---|---|
| AG-AC01 | edu/open/off/pro、pane 关闭找回/布局恢复 | 入口正确，pane 不重复，不复活被禁用功能；A |
| AG-AC02 | 真宿主 Ready→session→run，规则/模型失败 | UI 不阻塞；真实卡片与降级来源清楚；A |
| AG-AC03 | 飞行请求中切工程/保存/重启/关窗 | 迟到回包不污染新状态，无旧控件访问；A |
| AG-AC04 | DTO/未知卡/恶意链接/并发/事件分页与过期 | 非法内容不执行，去重与消费游标准确；A |
| AG-AC05 | canvas/RTL/wave/resource 交叉定位 | 当前证据准确，无循环；缺映射/历史不猜测；A/B 按适配能力 |
| AG-AC06 | 无课程/教学状态与真实六步映射 | 无虚构进度；B 由实际证据驱动并在改码后失效 |
| AG-AC07 | L1～L3 升级，直接/伪造/过期 L4 | A 不可用；B 两次真实操作、receipt 核销与负例全部通过 |
| AG-AC08 | 决策计划、审批、改参数、取消和重试 | A 只读；B 有界一次 Job，旧计划/审批拒绝，终态真实 |
| AG-AC09 | 概念卡 hover/键盘/缺词/屏幕边缘 | 600/300ms、焦点与关闭正确；C ≥50 审阅词条 |
| AG-AC10 | overlay/框选、大 tick、x/z、缩放与旧波形 | 坐标/单位/归属准确，不改原操作；扩展接口后才启用提问 |
| AG-AC11 | 实板三步、缺设备/配置、取消/失败导出 | C 有真实设备链和对齐证据，Agent 无 debug/flash 权限 |
| AG-AC12 | 历史查看、dirty/跨项目/损坏快照恢复 | A 只读；C 不丢用户编辑，恢复原子/可撤销，revision 与审批失效 |
| AG-AC13 | 浅深/100～200%/键盘/窄窗/长对话 | 可读、可操作、无重复窗口与回调泄漏；所有阶段 |

A 联调至少用当前真实 sidecar，覆盖规则模式、缺模型/断网、健康失败、旧会话 404、两工程切换、保存新 revision、事件补读和未知卡。Mock 结果单独标注。B/C 各自按对应 Gate 验收；PASS/FAIL/SKIP/未执行分开，硬件与原生平台缺证据不得标完成。

## 9. 交付和修订记录

交付为新增组件/适配器/测试与构建登记、使用说明、`acceptance.md` 和 task 状态。接口扩展在 `contracts/edu-agent/v1/CHANGELOG.md` 同步登记兼容影响，不仅修改界面文档。

v1.1 修订：统一主框架 theme/layout；按真实 splitter/波形位置定义四视图；区分 Agent 请求与 EDA Job；纠正 L4、工程模式、快照写回和实板默认权限；补异步 UI/同步 201 适配、上下文失效、证据来源、未实现能力与分阶段验收。原 T-1～T-8 的设计目标均保留。
