# D12：跨 revision 编辑与重新审批实施计划

日期：2026-10-16。

目标：在既有 D11 真实综合闭环后，由测试宿主模拟学生保存 RTL，确认旧审批不能启动新 Job；以新 revision/snapshot 和新基线建立独立教学会话，重新审批后完成真实综合与反馈。实现仍限定在 sigflowAgent；只扩展测试宿主，不增加共享 HTTP 写文件接口。

## 设计与取舍

采用新 revision 创建新会话，保留旧会话的不可变绑定。原地修改旧会话会使检查点、审批摘要与历史证据含义变化，不采用；等待 GUI 接线则不能独立复现本地验收。测试父进程只编辑自己创建的临时 RTL，通过自己持有的 Core stdin 请求刷新同一工程。Python sidecar 没有此管道，也不能选择文件路径。

执行适配器在授权与每步执行前读取当前工程 context，严格验证 project/revision、dirty=false、synchronized=true。过期或未保存状态在建立提交账本记录及 POST Job 前拒绝。Gateway 保留最终原子校验，处理预检后的版本竞争。已知 Job 的 observe/recovery 仍为只读，不依赖当前 revision 可执行。

入口增加 `--scenario same-revision|student-edit`，默认行为保持 D11。student-edit 使用规则模式完成附加轮次，避免额外模型费用；原主轮仍可独立选择 DeepSeek。最终报告区分两个 revision 的 Job/证据，综合成功继续使用 outcome_indeterminate，不推断功能修复。

## 约束

- 不提交、不推送；保留初次 stash `824925a13fd8cf79dfb559a392ef918c911fa100` 及包含 D12 的第二份 `e0b4b8bfd25640492df0d0ad3b9606bc35b7edfe`。
- 初期三次 pull 网络失败；网络恢复后第四次快进成功，当前 HEAD 为 8b067b2。拉取后的源码和报告必须重新验收，不能复用旧构建冒充兼容。
- 不清理已有文件或无关进程；仅清理由本次运行创建的临时工程及直接持有的子进程。
- 不改变共享 edu.api.v1；stdin refresh 只属于可选集成宿主。
- 不覆盖 D11 历史机器报告；D12 单独产出报告与源码摘要。

## Task 1：执行前当前版本门禁

文件：host/verification.py、tests/test_host_verification.py。

- [x] 测试先行：shared HTTP fixture 增加完整 project context；revision 改为 rev-new 后，authorized 返回 false、execute 抛 STALE_REVISION，POST 数和 verification_requests 行数均为 0。
- [x] 表格负例覆盖错 project、非布尔保存状态、dirty、未同步；未知结构返回 INVALID_RESPONSE，实际不可执行状态返回 STALE_REVISION。
- [x] 在 `_validate(plan, grant_ref, _approved=None)` 认证 health 后读取 context，先结构校验，再可执行性校验；不改变 observe 的 GET-only 路径。
- [x] 先运行新测试确认失败，再实现并运行 verification、observation、restart、coupled 回归。

## Task 2：真实学生编辑场景

文件：integration/edu_u2_gateway.cpp、host/closed_loop.py、新 host/revision_acceptance.py、host/acceptance_support.py、tests/test_closed_loop.py。共享验收原语集中在 acceptance_support，避免 CLI 以 -m 启动时产生两份 RunFailure 类型。

- [x] 首先新增行为测试：runner 将 student-edit 选项传给 exercise，默认仍 same-revision；通过实际子进程验证未知 refresh 命令安全关闭，不允许额外字段携带路径。
- [x] C++ 宿主 ready 后循环有界读取 JSON 行，仅接受 `{"type":"refresh-project"}`；刷新启动时固定 root，失败以非零退出，EOF 保持已有关闭行为。不输出额外 stdout。
- [x] exercise 保持默认流程，student-edit 时调用独立 `exercise_student_edit(core, project, route, gateway, ui, teaching, scope, plan, create, deadline)`，返回脱敏附加轮次结果。
- [x] 先建立旧版本待审批会话与未使用 grant；父进程保存不同 RTL，刷新 Core 并轮询 context；断言 revision、snapshot、input_fingerprint 均改变，旧 snapshot 请求与旧 Job 请求 409，旧教学执行失败且不消耗 grant、不创建 Job。
- [x] 新版本运行真实基线 Job；新教学会话先使用旧 grant 验证拒绝，再使用独立会话/新 grant 审批完成复测。检查新基线/结果各自绑定新 scope，证据不同、总 Job 4 个，重放不新增 Job；所有失败路径沿用 D11 清理与脱敏报告。
- [x] CMake 构建并运行实际 student-edit 规则场景；缺工具、旧宿主不能刷新、没有新证据均失败，不以 fixture 代替通过。

## Task 3：验收与交接

- [x] 独立审查本阶段差异并修正实际缺陷。
- [x] 运行最终全量测试、40 项开发评测与真实 D12 场景；保留 D11 默认场景回归证据。
- [x] 产出 D12 开发手册、机器报告，更新 task.md、README 及本计划执行记录；校验文档链接、摘要和 diff 空白。

后续仍独立验收真实 Verilator 仿真/波形、GUI、教学质量及硬件功能正确性，不开放 L4。

## 执行记录

Task 1 新测试在原代码上产生 10 项断言失败，实现后 55 项执行适配、31 项观察/恢复、5 项组合测试通过。已知 Job 在当前 revision 改变后仍可只读查询。

Task 2 新 CLI 用例先因缺少 scenario 参数失败；真实旧宿主在刷新时关闭，实际场景明确失败。实现后 8 项运行器测试通过，CMake 构建 edu_u2_gateway 成功，真实 student-edit 场景完成 4 个 Yosys Job；默认 same-revision 场景完成 2 个 Job。未知控制命令和带额外 project_path 的刷新命令均在真实子进程中拒绝。

拉取前全量 426 项：420 通过、6 项历史可选 UCAgent 实验跳过，57.452 秒。两阶段独立审查均通过，组合未发现阻断缺陷。审查额外确认缺失 context 字段拒绝、授权后状态改变拒绝、跨版本重启后的只读观察，以及真实宿主 4096 字节合法命令/超长/无换行截断/EOF 边界。中途失败可能丢失部分阶段诊断事实，已作为非阻断限制写入手册，不影响失败关闭。

网络恢复后快进到 8b067b2。合回 D12 时保留远端的字符串/JSON 类型修正，并将 RAG 忽略规则与 .local/ 取并集；没有遗留冲突或暂存条目。首次新基线全量发现 CloseoutTests 硬编码 D: 原始快照路径，只修改该日常集成测试的 setup，使用仓库数据库临时副本且提前注册清理，9 条测试断言全部保留并通过；原始快照重建测试仍按原依赖独立运行。最终全量 482 项，447 通过、35 跳过，68.298 秒；原 agent 独立 220 通过、1 Windows 平台跳过；开发评测 40/40。CMake 重建以及两类真实综合场景均在新基线上重跑通过。源码与报告摘要见 [D12 汇总](closed-loop-d12-verification.json)。
