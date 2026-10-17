# U1 完整闭环实施计划

日期：2026-10-15。范围：补齐 Python 教学宿主及其真实 Core/Gateway 联调入口。

## 设计与验收边界

闭环定义为：启动真实 Core Gateway 和 Python sidecar，取得真实工具基线报告，建立可信证据，创建教学验证会话，展示计划并由测试宿主显式审批，签发 scoped grant，提交真实 Job，观察终态，读取新报告并输出有证据引用的反馈。规则模式用于确定性集成测试；DeepSeek 模式使用本机配置单独验收。工具成功只证明工具任务完成，不证明设计功能正确。

继续沿用现有 loopback 身份隔离、只读恢复、固定项目/revision/snapshot、持久幂等与未知回执禁止重提的边界。共享 edu.api.v1 与私有 u2.host.v1 保持各自版本。

## 全局约束

- Python 服务位于 sigflowAgent；保留 agent，两个同名 Python 包不在同一解释器导入。
- 不提交、不推送，不删除已有文件或工作区外文件；新增测试只清理自己创建的临时目录。
- 恢复不能保存或重放 grant、token、原始 question、模型输出执行指令；恢复只能重新取证和 GET 已登记 Job。
- 验证恢复不允许启动后继 Job；后继执行必须重新显式审批并提供有效 grant。
- 真实工具缺失、报告不完整、证据不匹配必须明确失败或报告缺失条件，不能以 fixture 成功替代真实通过。
- 每阶段记录测试命令、结果和未覆盖边界，独立审查后更新 task.md。

## Task 1: 验证会话重启后的只读恢复

新增独立且严格有界的验证检查点格式，不改变只读 checkpoint.v1。保存恢复所需的计划和审批绑定、执行索引、运行 ID、预算/版本及可信证据摘要，不保存任何授权凭据或原始问题。与会话状态同事务写入；重启必须 RecoveryRequired。

已有 recover HTTP 入口继续接收原始 question 用于摘要校验。验证恢复重新构造相同绑定，重新读取基线和已完成步骤证据，凭持久验证账本只读观察当前步骤，保留未知回执与停止事实。不能自动调用 authorized/execute，不能调用模型；后继步骤回到 WaitingApproval。对未发出的计划可恢复至 WaitingApproval。重启后取消通过恢复的精确审批绑定只取消本会话拥有的已知 Job。

TDD 覆盖：跨进程/重新打开 store 的 pending→成功、无新 POST/模型调用、问题/计划/范围篡改拒绝、结果证据迟到、未知回执不重提、后继需要新审批、取消隔离、版本单调和敏感字段不落盘。保留既有 readonly 恢复、幂等、取消行为。

## Task 2: 可信证据生产与无诊断结果

通过认证 Gateway 的结构化 artifact 元数据、报告绑定及 hash 构造锚点，禁止由诊断自由文本推测源码位置或归属。新入口必须固定项目/revision/snapshot/Job，校验同实例 health、报告完整性和 artifact 的所有权/hash；保持外部可信目录模式兼容。

结果报告可能没有诊断。支持有 artifact 证明的有界任务完成观察；该观察不能声称功能正确或凭诊断消失推断修复。反馈保持 outcome_indeterminate，除非另有可信 expected_resolver。补充错范围、hash、无 artifact、未完成、空诊断及正常诊断测试。

接口收敛：增加显式 `--gateway-evidence` 模式，与 `--anchors`/环境目录互斥。可信 Gateway 目录使用认证的 `GET /artifacts/{id}` 核验报告中的 artifact 所有权及 hash，实时生成稳定锚点且不缓存失效结果。保留外部目录模式作为默认值。预留 `diagnostic_id=job-report` 表示完整结构化报告的任务观察，concept 为 `job_observation`，只描述任务状态及报告范围，不伪造诊断、源码定位或 detector 结论。该观察要求 core 来源、完整报告、非空 input_fingerprint、Succeeded、exit_code=0、受支持能力/provider、非空且核验的产物。新结果仅在同一显式目录模式下使用该观察；已有目录的诊断选择规则不变。

模型门禁收敛：现有策略仅对 supported pattern 调用模型。增加独立的中性任务报告教学模式，允许完整、可信且带锚点的 `job_observation` 在 L1–L3 解释工具任务边界；该模式不是第九类硬件错误 detector，不改变八类错误模式的门禁。规则提示与模型上下文均保留“综合成功不证明功能正确”的范围，未核验/过期/缺证据和其他未知 concept 仍不能开启模型。TDD 证明该可信模式实际调用 Provider，坏证据/未知模式不调用。

## Task 3: 真实闭环集成入口

增加隔离临时工程的 headless Core/Gateway 测试宿主和可运行测试脚本，使用现有官方 Yosys 插件、真实 HTTP、真实 sidecar 进程、UI/Agent 不同 token、snapshot、计划审批和 grant。不依赖 UCagent，不修改用户工程，不把 Gateway 签发 grant 的 UI token 传给 Python sidecar；sidecar 仅接收 Gateway Agent token 和用于自身接口认证的独立 Teaching UI token。

测试入口默认规则模式；可选 DeepSeek 模式不允许静默降级，缺 Key 明确报错。断言完成反馈、新 Job/新证据、正确状态/版本、取消/幂等边界，输出无凭据机器可读报告。真实工具缺失不能计为通过。提供开发手册说明构建、运行、预期输出和剩余 GUI/教师验收边界。

构建收敛：新增默认 OFF 的 `SIGFLOW_BUILD_U2_INTEGRATION`，在 `sigflowAgent/integration` 生成 `edu_u2_gateway`，复用现有 Core/Gateway/官方 Yosys 插件；不依赖旧 agent 服务及 wx 界面。C++ 宿主从 stdin 读取凭据和隔离工程路径，只输出无凭据 ready 行；父进程持有 UI 控制身份，Python sidecar 仅取得 Gateway Agent 身份。runner 使用自己的 TemporaryDirectory 和随机 token，实际跑两个 synthesis Job（基线与审批后的复测），校验 grant 消耗及动作重放，不创建用户工程。测试输出必须标明 real_core/real_yosys、模型模式及功能判定未覆盖。

兼容收敛：真实 Core 的 Verilator build 插件为 `eda-sim-verilator`，run 插件为 `eda-sim-verilator-run`。执行适配器须按 capability/provider 精确映射，不能把两者合并为相同插件。更新能力、提交、观察、取消与 build 依赖检查，以及使用旧合并 ID 的测试 fixture；错误 provider 继续拒绝。编译预检发现的 C++20 字符串/JSON 比较错误一并修复。集成验收主路径为真实 Yosys；真实 Verilator 仿真、跨 revision 学生修改流程、GUI 和教学正确性不以综合任务闭环代替签收。

真实运行补充：首个实际综合成功后，报告 plugin_id 为 null，定位到 Gateway 仅解析能力但未填入 JobRequest.pluginId。提交路由须写入已解析的可信 provider，并在任何 grant 消耗或幂等预留前拒绝显式不匹配或类型错误的 plugin_id。缺省字段继续由 Gateway 解析，不能用客户端声明代替真实来源。真实回归检查报告 provider、错误请求拒绝及授权消耗不变。

## 执行记录

2026-10-15 三项任务均完成并通过独立审查。Task 1 的跨运行取消隔离问题已修复并复审；Task 3 最终审查发现的清理失败误报成功、管道关闭期限和报告写入异常均已修复，7 项定向回归及限定范围复审通过。

最终全量 421 项：415 通过、6 项历史可选实验跳过；旧 agent 独立回归 124 项通过；开发评测 40 项通过。真实规则模式和真实 DeepSeek 模式均完成两个真实 Yosys Job、审批、可信新证据及反馈；DeepSeek 实际调用 1 次，初始卡来自模型。复现命令及机器报告见 [D11](u1-d11-closed-loop.md)。未提交或推送。

后续再规划：真实 Verilator build/run、跨 revision 学生修改与重新审批、GUI 和教师验收分别形成后续阶段；当前综合闭环不替代这些范围，也不自动开放 L4。
