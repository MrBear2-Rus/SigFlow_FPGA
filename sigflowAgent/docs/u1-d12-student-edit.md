# U1 D12：学生保存新版本与重新审批

日期：2026-10-16。实施范围与步骤见 [D12 计划](u1-d12-revision-plan.md)。本阶段补齐跨 revision 的真实综合工作流；不改变旧教学会话绑定，也不将综合结果解释为功能修复。

## 仓库同步状态

本地改动和未跟踪开发文件先保存为 stash `824925a13fd8cf79dfb559a392ef918c911fa100`，说明为 `codex: preserve U1-U2 closed-loop work before 2026-10-16 pull`。初期三次拉取遇到 GitHub 连接重置或 443 连接失败，恢复原工作后继续完成 D12。网络恢复时再保存含 D12 的 stash `e0b4b8bfd25640492df0d0ad3b9606bc35b7edfe`，第四次快进拉取成功，由 ecb1188 更新至 `8b067b2fdabf9abdbe63764dfcf5259be88170fd`。

第二次 stash apply 出现两处冲突：Gateway 保留远端的 std::string/JSON 类型修正，同时保留本地可信 provider 赋值；.gitignore 保留全部 RAG 规则并追加 .local/。已无遗留冲突，开发文件最终保持未暂存。两份备份均保留；加密凭据、虚拟环境及本地运行数据不纳入 stash。没有提交或推送，没有清理无关文件。

## 执行边界

GatewayVerification 在认证实例后读取 `/projects/{project_id}/context`，要求相同项目、相同 revision、严格布尔的 `dirty=false` 与 `synchronized=true`。结构错误返回 INVALID_RESPONSE；实际版本改变或未保存返回 STALE_REVISION。校验在读取 grant、预留提交记录和发送 Job 前完成，授权检查返回 false，直接 execute 抛固定 GatewayError。预检后发生版本竞争仍由 Gateway 的最终保存状态、revision、snapshot 校验拒绝。

observe 与重启后的已知 Job 查询不增加当前版本执行门禁，因此旧 Job 可继续按其原始 revision/snapshot 只读取证。新版本需要新 snapshot、新基线、新教学会话及独立审批；不改写历史计划、检查点或授权材料。

集成宿主增加父进程 stdin 控制命令 `{"type":"refresh-project"}`，最大 4096 字节，只刷新启动时固定的测试工程。额外字段、其他命令、截断或过长行返回非零退出并关闭服务。不增加共享 HTTP 接口；sidecar 不持有该管道，不能请求保存文件或切换目录。ready 后不输出额外 stdout，关闭、日志排空和脱敏失败规则沿用 [D11](u1-d11-closed-loop.md)。

## 构建与运行

仓库根目录 PowerShell，沿用既有编译器与依赖缓存：

```powershell
cmake -S . -B build-main-msvcrt -DSIGFLOW_BUILD_TESTS=OFF -DSIGFLOW_BUILD_EDU_AGENT=ON -DSIGFLOW_BUILD_U2_INTEGRATION=ON
cmake --build build-main-msvcrt --target edu_u2_gateway -j 4
$env:PYTHONPATH=(Resolve-Path sigflowAgent).Path
& .\sigflowAgent\.venv\Scripts\python.exe -P -m sigflow_edu_agent.host.closed_loop --gateway build-main-msvcrt/sigflowAgent/integration/edu_u2_gateway.exe --mode rule --scenario student-edit --timeout 120 --output sigflowAgent/docs/closed-loop-student-edit.json
```

默认 `--scenario same-revision` 保持 D11 两 Job 场景。student-edit 在原场景之后追加一轮，全部工程与 RTL 均位于运行器自己的临时目录；不读取或修改开发者工程。依赖仍为 Python >=3.11 标准库、可构建的 Core/Gateway、官方 Yosys 插件与真实 Yosys，不要求 Conda 或 UCAgent。新增 RAG 日常检索使用随仓库的 data/course.sqlite，要求 SQLite 支持 FTS5 trigram；原文重建需另外提供固定快照及构建依赖。

student-edit 的新增轮次始终使用规则模式；即使主轮选 `--mode deepseek`，也不会为新增会话额外请求模型。本阶段实际运行使用规则模式，未再次调用真实 API。D11 的历史真实 DeepSeek 验收记录保持独立，不替代本阶段真实工具证据。

## 实际验收断言

1. 原 revision 完成两个真实综合 Job；额外建立旧版本待审批会话和未使用 grant。
2. 测试父进程将临时 RTL 的 `q <= d` 保存为 `q <= ~d`，请求固定工程刷新；revision、snapshot 和输入指纹改变。这是可观察的编辑，不声明哪种逻辑符合设计目标。
3. 旧 revision 的 snapshot 创建与 Job 提交返回 STALE_REVISION；旧教学会话执行失败，原 grant 使用量为 0，Job 总数仍为 2。
4. 新 revision 创建真实基线 Job。新教学卡使用旧 grant 仍被拒绝，Job 总数保持 3；随后创建独立会话并签发新审批，才启动第 4 个 Job。
5. 新基线与复测报告均绑定新 revision/snapshot，输入指纹与旧版本不同；新证据不复用旧证据，最终 Completed/outcome_indeterminate。重放审批动作不增加 Job；五个 grant 的使用量依次为 `[1,1,0,1,1]`，临时目录与拥有的子进程清理完成。

真实场景已通过，记录见 [跨版本机器报告](closed-loop-student-edit.json)。默认两 Job 场景也已回归通过，见 [同版本机器报告](closed-loop-d12-same-revision.json)。另以真实宿主子进程分别测试未知命令及带额外 project_path 的刷新命令，两者均返回 5，未输出额外 stdout，拥有的子进程均已退出。

失败报告只用于拒绝本次验收，不保证列出中途已发生的全部执行事实；尤其新增轮次中途失败时，可能不包含已经完成的前一轮摘要。不能用失败报告中的 job_count 或 real_core 默认值反推没有发生工具执行，实际 Job 事实以 Gateway 查询为准。该限制不改变失败退出码、清理或禁止自动重提未知请求的规则。

## 复测与剩余范围

```powershell
$env:PYTHONPATH=(Resolve-Path sigflowAgent).Path
& .\sigflowAgent\.venv\Scripts\python.exe -P -m unittest discover -s sigflowAgent/tests -v
& .\sigflowAgent\.venv\Scripts\python.exe -P -m sigflow_edu_agent.evaluation --suite sigflowAgent/tests/evaluation/development.json
```

定向回归：55 项执行适配测试、31 项观察与重启测试、5 项组合宿主测试、8 项运行器测试通过。开发评测输出单独保存为 [evaluation-d12.json](evaluation-d12.json)，属于合成开发场景。

拉取前全量为 426 项。新基线最终全量 482 项，447 通过、35 跳过，耗时 68.298 秒；跳过项为 6 项历史 UCAgent 实验及 29 项需要未附带原始快照的 RAG 重建场景。原 agent 最新独立回归 221 项，220 通过、1 项 Windows 不支持的 SIGTERM/SIGINT 场景跳过。开发评测 40/40 通过，源码摘要匹配。全量用例中刻意触发缺宿主错误的回归会输出 gateway_missing JSON，应读取最终 unittest 汇总及独立真实场景报告判断结果。

新增 CloseoutTests 原 setup 硬编码了 D: 盘原始快照，在本机表现为初始化错误。日常集成测试现将随仓库的课程数据库复制到测试自己的临时目录，注册 class cleanup 后运行原有 9 条断言，全部通过；没有修改数据库、课程事实或放宽断言，也不将这些测试冒充原文索引重建验收。仓库数据库 SHA-256 保持 `4a98cecdc34a16446c3a5860072db18776871f437c329b2c62cda153fe662b01`。独立宿主尚未默认装配 CourseProvider，真实综合场景不宣称已覆盖真实工具与课程联合展示。

执行门禁和集成场景分别通过独立审查，组合检查无阻断发现。审查另实测缺失字段、授权后工程变更、跨版本重启只读观察，以及真实宿主 4096 字节合法刷新命令、超长命令、无换行截断和 EOF；边界均按预期处理。最终源码、可执行文件与报告的摘要、同步状态及验收范围见 [D12 汇总](closed-loop-d12-verification.json)。

本场景覆盖的是测试宿主模拟保存后的跨版本综合流程，尚未覆盖 GUI 的编辑/保存/图码同步时序、跨版本自动诊断关联、真实 Verilator 仿真、波形、硬件功能正确性、教师教学质量或 POSIX 原生运行。这些范围继续单独验收；L4 仍默认拒绝。
