# U1 正式宿主中的 U2 教学组合实施计划

计划日期：2026-10-21；收尾验证：2026-10-22。基线 04648a9；恢复 stash 81d6380d 中的 U2 阶段 20–31，保留全部 stash，不提交或推送。远端拉取再次连接重置，未宣称基于未取得的新提交。

**Goal:** 在 U1 已有进程、端口和认证边界内装配 U2，补齐持久关联、教学等待/动作/事件、取消、显式恢复及真实当前工程证据的可验证调用链。

**Architecture:** 选择同进程可选组合。U1 新增 `/api/v1/u2/sessions` 和 `/api/v1/u2/runs` 教学扩展，外层仍使用 edu.api.v1 envelope，扩展数据明确标注 u1.u2-teaching.v1；原有共享基础路由保持语义。U1 SQLite 只保存外部身份、工程范围、幂等摘要及 TeachingHandle，不复制 U2 状态机/卡片/事件。U2 Runtime 继续拥有动作、预算、checkpoint、取消和恢复。共享基础 agent-run 没有等待态，扩展返回 phase/pending 和 run=null，只有真实 Completed 才提供冻结 run。

**Tech Stack:** Python 标准库/SQLite；U1 原基础模式保持 Python >=3.9，启用 U2 需要 Python >=3.11 和独立安装的 sigflow_u2；运行时不新增第三方依赖。

## 范围与取舍

替换 U1 调度器会破坏已有一次性接口；HTTP 转发另一 sidecar 会增加双进程生命周期和双端口问题。本阶段采用显式开关 `--u2-teaching` 在现有服务器注册扩展。源树组合由可信启动器设置两个源码根；正式安装同时安装两个独立分发。普通请求不决定插件路径、数据库路径或接收器工厂。

沿用用户指定的当前工作目录和分支，不另建 worktree。允许修改 agent 中 U2 必需的组合接线，保留 U1 原基础工作与原测试。main/Core/共享冻结 schema/demo/U3 数据和存储不改。没有真实材料或教师认证时 HTTP L4 继续拒绝；实际 UA-10、材料/receipt 通过可信工厂参数注入，不从请求动态加载。

## 阶段 32：外部关联与可信服务

文件：新增 `agent/src/sigflow_edu_agent/teaching_store.py`、`teaching.py`；新增 `agent/tests/test_teaching_store.py`、`test_teaching_service.py`。

接口：BindingStore(path, instance_id)，create_session(learner, project, revision, key)，reserve_run(sid, learner, key, request_sha256)，attach(rid, handle)，session/run 的同 owner 读取和 close。TeachingService(runtime, bindings, prepare, adapter_factory, check_current) 委托真实 U2 端口。

- [x] 失败回归：模块缺失；同 key 同范围复用 sess/run；同 key 改输入 409；跨 learner 403；重开数据库保留 ID；禁止原问题/凭据/卡片正文入关联库；session/run 限额和 instance 隔离。
- [x] 最小实现 SQLite 事务及预留关联，先提交稳定外部 run 再调用 Runtime.create，断点后仅相同请求显式重试；不后台重放模型或 Job。
- [x] 真实 U2 Runtime 组合测试：L1→明确 hint_next→L2、重放、取消、重启 RecoveryRequired→显式 recover；外部 I/O 不占关联库全局锁。
- [x] 产出阶段 32 文档，核对实际 gateway 当前版本与 report/snapshot/artifact 证据的组合边界。

## 阶段 33：U1 HTTP 与启动生命周期

文件：新增 `teaching_routes.py`、`teaching_factory.py`；修改 `agent/src/sigflow_edu_agent/http_server.py` 的可选配置与注册、`__main__.py` 的显式开关/关闭；新增 `agent/tests/test_teaching_http.py` 和进程测试，新增可信源树启动器 `sigflowAgent/bin/sigflow-shared-u2.cmd`。

路由：POST sessions（幂等键）；GET session；POST session/runs（幂等键）；GET run；POST run/actions；POST run/cancel；GET run/events。全部位于 `/api/v1/u2`，全部先执行 U1 现有 Host/Origin/Bearer 检查，再要求恰好一个 X-Learner-ID。动作 expected_state_version 为当前十进制字符串，reference/L4 不接受 HTTP 注入。

- [x] 先增加真实 HTTP 失败测试，验证 capability 明确声明、等待态 202 与 GET 200、卡片使用冻结 schema、身份/版本/参数拒绝、取消和显式恢复、事件游标与终态。
- [x] 注册同一个 U1 server；生产 factory 只使用 bootstrap 路径/身份，GatewayEvidenceCatalog 和现有 verification/course/wave 组合；读取前后核对当前工程版本。
- [x] 事件仅投影 sequence/version/status/action 元数据，加外部 sess/run 身份，不暴露内部会话、正文或原始运行时结果；显式标注扩展事件，不冒充冻结 agent-event。
- [x] 启动缺少 U2 时失败闭合，基础模式无新增依赖；ready 仍只有一行，结束关闭 U2 工作者、存储与验证适配器。产出阶段 33 文档及兼容说明。

## 阶段 34：真实工程联合验证与交付

文件：新增 `sigflowAgent/sigflow_edu_agent/host/shared_acceptance.py`、相应测试和阶段文档/机器记录；更新 task.md、交接状态和依赖矩阵。

- [x] 在自有临时工程启动真实 Core/Gateway 和正式 U1 `--u2-teaching`，通过同端口扩展取证、L1/L2/幂等/取消、真实审批和验证反馈；规则模式 0 次模型调用，不能用 fixture 代替工具证据。
- [x] 重启 U1，验证外部 ID/关联保存、旧卡隔离和显式恢复；不同实例不合并；未审批无额外 Job。
- [x] U1 完整原有与新增测试、U2 完整回归、开发评测、独立只读审查及文档链接检查。只在相关新增改动或失败时重复运行。
- [x] 归档源码/二进制摘要、跳过原因和失败记录。区分 U1/U2 已实现连接与真实 UA-10/材料/教师/其他 detector/UI/平台/冻结集验收，未到位外部输入保持未完成。

每阶段完成后记录实际结果并再规划；受用户“不提交/不推送、保留目录和 stash”的约束，不执行技能模板中的提交步骤。

## 阶段完成后的再规划

阶段 32 完成外部关联和可信组合服务，U2 保留唯一工作流日志；中断 attach 后只能同请求显式重试，重启不自动执行模型或 Job。后续通过正式宿主端口消费，不复制状态机。

阶段 33 完成 U1 原服务器内的显式教学扩展、元数据事件、启动/关闭和可信工厂注入；补齐提前退出及关闭异常的资源释放回归。后续由 UI 消费扩展；实际 UA-10/材料/权限通过可信工厂组合，不从 HTTP 注入。工作者停止等待有上限，不宣称所有外部 I/O 均已优雅结束。

阶段 34 完成正式 U1 与真实 Core/Yosys/Verilator 的综合/仿真联合验证、重启恢复、L1–L3、审批/后继、预测和版本隔离。事件元数据的新增断言由最新仿真报告验证，较早综合报告保留当时范围。

U1 305 项、1 条平台跳过；U2 719 项、37 条跳过记录；两套失败/错误均为 0，开发评测 40/40。共享预测/动作定向 27 项通过，U1 新教学组合定向 16 项通过；独立只读审查的文档证据范围意见已修正。上次 U2 最终回归因自动审批服务额度错误未执行，2026-10-22 在相同审批路径重试完成，未绕过审批。机器摘要见[验证记录](verification-stage32-34.json)。

现有依赖下必要 U1 接线不再阻塞 U2。正式教学扩展仍需团队接收及原生 GUI 消费；实际 UA-10、L4 材料/教师权限、额外 detector/行为契约、冻结输入/真实模型批次、双教师评分和目标平台验收仍保持未完成。不制作替代生产数据，不提交、不推送，stash 全部保留。
