# Phase 1 扩展外设与四大故障实测报告

日期：2026-11-17。工作分支：`feat/virtual-fpga-phase1`。基线：`988570e709abe0b13037028063490c7c1ba468a4`（含 M0 收官 `5eea59f`）。Web 暂缓。

当前收官状态以 [P1-acceptance-summary.json](evidence/P1-acceptance-summary.json) 的 `status` / `Phase1_complete` 为准。历史 [M0-final-acceptance-summary.json](evidence/M0-final-acceptance-summary.json) 与 K0～K5/U0/U1 归档独立保留。

**本轮结果：status=passed，Phase1_complete=true。**

| 验收项 | 最终固定源码上的结果 |
|---|---|
| 桌面构建与 CTest | 37/37；保留原 34 项，新加 3 项；CTest 共 334.98 秒 |
| 四故障内核 / 真实 runner | 137 / 43 项断言通过，含配置热加载独立流与原始接点轨迹 |
| 外设内核 / 真实 runner | 10 / 38 项断言通过，含 27MHz PWM、共阳/共阴、DIP、8N1/7E2 与错误恢复 |
| 新增 P1 GUI | 36 项断言；真实 wxListCtrl 错误行 danger Token + 文本已检视；20 次生命周期通过 |
| GUI 从零重放 | 589 commits，三次 circuit/control digest 一致；各真实 runner 场景也分别三次核验 |
| M0 产品回归 | U1 原 A1～A10 全部通过，本轮 497 项断言；历史 437 项收官归档保留 |
| 证据冻结 | 源码前后 SHA-256 一致；60 份 M0 历史文件哈希不变；27 份 P1 证据与 gzip 解压字节哈希已核对 |

此前一轮虽已 37/37 通过，但验收期间仍补充了排针资源分类、热加载运行态保留与 GUI 高亮检查，因此当轮未设置 Phase1_complete；本结果来自上述修改完成后重新执行的完整构建与 37 项 CTest。不是把混合源码版本的结果拼接为收官。

## 1. 实现与运行边界

| 层 | 生产实现 | 边界 |
|---|---|---|
| 故障 | Phase1Faults，严格加载 sigflow.fault-profile.v1，独立 PCG32 | 拒绝未知字段、错 hook/target、浮点概率/次数、冲突窗口；四类故障，不增加 P2 模型 |
| 运行 | VirtualSession 显式 phase1=true 转到 Phase1Session；无 wx 依赖 | P1 使用新版本协议；未开启 P1 的 M0 会话走原调度实现 |
| 外设 | Phase1Peripherals / DutyIntegrator | 整数时间积分；真实 27MHz 每个所需边沿执行 DUT；UART 由自己的位期限驱动 |
| GUI | VirtualBoardPanel / BoardRenderer / FaultInjectionPanel / VirtualUartPanel | 同一控制器、生产构建器、持久子进程、LiveTrace；绘制和轮询不改变模拟状态 |
| 数据 | registry 板卡/electrical/pins v2、schema、示例工程 | 通过核准 Pad + 显式 connections + CST；没有直接按 HDL 名驱动外设的捷径 |

扩展运行以 `phase1` 显式选择版本，避免给既有 M0 日志偷偷更换语义。P1 新增 core 编译单元、生成 runner 使用同一静态库，GUI 不拥有 DUT。当前只有一个声明的主时钟；数字模型不模拟模拟电流、器件延时或亚稳态。外部适配器若要求未实现的额外 settle 回调，返回有界 E_NON_CONVERGENT；生成的 Verilated 模型内部 settle 仍按既有实现执行。

## 2. 四大故障与随机流

| 故障 | 机制 | 验收观察 |
|---|---|---|
| Stuck-at replace_source | 修改指定 dut: 或外设模型源，保留其他源 | DUT 原始输出继续变；不制造额外 CON；撤销重新构建原始驱动集 |
| Stuck-at short_to_rail | 保留原源，额外注入 SLO/SHI | 反向强驱动由 Resolver 产生真实 CON；同电平不产生伪冲突 |
| Open | 在 resolve 前断开 connection edge | Pad 和器件端独立，输出支路不阻塞 DUT；transient 使用 [start,end) |
| ButtonBounce | 用户 press 匹配后展开精确 relative_edges | 10ms 序列原始计数 3 / 消抖计数 1；release 立即取消剩余；pin.edge 不递归匹配 input.intent |
| ClockStop | 移除时钟期限并保持 hold_level | rising Step 有界 E_NO_CLOCK_EDGE；N Ticks / Next Event 仍可推进 UART、故障边界；结束后 restart_half_period |

永久/时间窗适用于 Stuck-at/Open/ClockStop；本阶段 intermittent 匹配器冻结为 ButtonBounce 的 input.intent/press。无法兑现的组合明确拒绝。相同资源的启用重叠窗口采取保守拒绝；不会让最后载入规则静默覆盖先前规则。所有规则仍受目标与电气绑定校验约束。

`pcg32-v1` 冻结为 PCG-XSH-RR 64/32，乘数 6364136223846793005，uint64 模运算。种子派生输入 UTF-8：`sigflow.pcg32-v1\n{十进制 seed}\n{stable_fault_id}`，SHA-256 前 8 字节按 LE 为 initstate，后 8 字节按 LE 为 sequence；inc=(sequence<<1)|1，预热、加 initstate、再预热。Bounded 使用 rejection sampling。禁止使用标准库随机分布器。

seed=20261109、fault_id=bounce-key3 的独立 Python 黄金向量：1466714255、2114244563、3207404460、3389275810、2947092696、3931443481。测试验证新增无关故障不消耗原流，且真实 Pad3 每条接点轨迹一致。随机判定、状态、触发数、剩余边沿进入 Journal 并重新计算核验。

配置热加载时，全局 rng 配置未变且规则未修改，则保留该规则完整运行态（包括 RNG、触发次数、冷却与剩余边沿）。同 ID 已修改规则保留随机流位置，但重新建立其调度状态；修改全局 seed 则显式开始一组新流。追加无关规则的原生测试核对正在进行的抖动检查点与下一期限完全不变。

## 3. 外设、电气与积分

| 项目 | 配置与实测 |
|---|---|
| LED PWM | 真实 27MHz，参考 RTL 输出 25%/50%/75%；4µs 滑动窗，8µs 后精确 duty_ppm=250000/500000/750000；15 次只读轮询不改变值 |
| 七段管 | 4×8 积分通道，位有效且段有效才累计；显式 common/active_level，共阳与共阴参考均验证 1234；瞬时只导通当前位；扫描周期取 digit0 有效沿 |
| DIP | bit0/1 → Pad40/41 → CST → DUT dip[0/1]，再改变 LED4/5；初始值与强驱动归内核管理 |
| UART | UART.TX → Pad18 → DUT 实际组合回环 → Pad17 → UART.RX；921600/8N1，另验 7E2；整数半位采样，错误停止位/CON 真实进入帧错误并在下一帧恢复 |
| 无效电平 | 瞬时 valid/contention/state_code 与 duty 分开；FLT/CON 不伪装为正常亮度；保留 unknown_ticks 便于检视积分窗口中无效区间 |

新增排针均核准为 Bank2 / 3300mV / LVCMOS33，来源为仓库已归档官方 6202 原理图与 UG803 QN88P（PSRAM）列，包含新增物理页 8；证据 SHA-256 与页码存于 v2 profile 的 provenance。核准属于文档交叉核对，没有声称已做真实板卡测量。

| 用途 | 段/位/开关顺序 | 物理 Pad（非排针序号） |
|---|---|---|
| 段 | a,b,c,d,e,f,g,dp | 25,26,27,28,29,30,33,34 |
| 位 | dig0,dig1,dig2,dig3 | 35,36,37,38 |
| DIP | bit0,bit1 | 40,41 |
| UART | FPGA TX,RX | 17,18（K1 已核准） |

排针连接关系为 J5.5/6/7/8/9/10/11/12 → Pad25/26/27/28/29/30/33/34，J5.14/3/2/1/13/15 → Pad35/36/37/38/40/41。模型显式声明允许实例扩展与必需连线；未核准 Pad、缺失端点均拒绝。实验不连接外部 TF/LCD 设备，不把这些附加接口当作第二个隐含驱动。

10ms 抖动快速验收使用单独声明的 **1kHz 教学时钟夹具**，测试会记录该身份；默认 P1 GUI 板仍为真实 27MHz。夹具不折叠周期，也不宣称已经以 27MHz 跑满 10ms。需要在真实板频跑长时段时，应按资源预算分段推进。参考 RTL 的非零复位寄存器先在复位保持期间采样一个实际时钟边沿，再释放复位和施加 press。

## 4. GUI 与确定性重放

实验箱增加 P1 工程快捷入口；无需手工选择 runner。Step 下拉三种模式，空格使用同一路径。观测下拉默认 Running 积分 / Paused 瞬时，也允许显式选择。故障面板 JSON 可加载、编辑、保存，双击规则启停，Stuck-at 可切换物理模式，显示 pending/active 与抖动计数。

底部「虚拟串口」提供 ASCII/Hex、四种行尾、按 tick 排序的双向记录、错误文字与主题 danger 高亮。输入只排外围队列；清屏只隐藏已有显示行，不清除内核记录，导出保留完整历史。所有面板析构取消会话/主题订阅；验收包含 20 次 P1 会话与临时串口/故障面板生命周期。

P1 协议：`sigflow.session-journal.p1.v1` / `sigflow.scheduler.p1.v1` / `sigflow.sha256-chain.p1.v1`。每个 commit 附带 p1_event（tick、evaluate、cause、actions）与重算检查点，包括 RNG 决策、时钟余数、积分、UART FSM/帧。Replay 创建全新模型，在独立 worker 从零重走调度事件并比较完整 commit，随后比较 circuit/control 两条 digest；直接篡改检查点会拒绝。旧 M0 日志版本与 digest 保持原语义。

GUI 验收宿主为真实 wxApp，组装生产 ProjectSessionBuilder、SessionController、Board/Fault/UART/Trace 面板；MainFrame 中实际新增底部串口页签。Light/Dark 与 96/144/192 的向量表面快照归档，属于注入 wx DPI 验证，未声称做过物理显示器切换。

## 5. 验收与复现

| CTest | 内容 |
|---|---|
| 原 34 项 | M0 A1～A10、旧 sim.build/sim.run/VCD、CST/电气、会话、LiveTrace/ThreadSanitizer、U0/U1 GUI |
| p1_four_faults_and_rng_replay | 原生模型边界与黄金向量 + 真实 runner 四故障/10ms 抖动/窗口/恢复；各成功运行三次从零重放 |
| p1_peripherals_pwm_seg_uart | 原生整数积分/图校验 + 真实 27MHz PWM/共阳共阴扫描/DIP/位级 UART/帧错误与恢复/重放 |
| p1_gui_mvp_acceptance | 真实 wxApp 控件交互、故障模式/按钮抖动、三种 Step、外设视图、串口导出/错误/清屏、三次重放、20 次生命周期 |

统一入口：`python tools/virtual/verify_phase1_final.py`。该入口顺序执行全部 CTest（-j1），给 JobService 临时目录独立根，避免历史并行测试共享 sigflow-jobs 文件导致的交叉覆盖。完整命令、退出码、计时、逐项结果与源文件 SHA-256 在 P1 summary；只有构建/测试成功、源码前后哈希一致、全部 M0 归档逐文件哈希不变才设置 Phase1_complete。

原始 P1 证据在 evidence/P1：构建/CTest 日志与 JUnit、原生/真实 runner/GUI 断言、串口帧、截图，以及有代表性的原始 Journal gzip。压缩文件记录压缩与解压字节 SHA-256，便于独立审阅。工作目录中的全部构建缓存、transcript 与未选取的大日志由 .gitignore 排除。

## 6. 当前范围与后续

保留的限制：单主时钟、数字功能模型、声明式固定四位扫描模型、有限事件/队列/积分区间配额；没有实时倍率承诺。可扩展更多实例的内核不等于当前固定 Tang Nano 面板已支持任意数量器件自动排版。课程加密/权限、完整探针/TraceBridge 教学题库、CDC/协议故障全集与 Web 映射仍属于后续阶段。
