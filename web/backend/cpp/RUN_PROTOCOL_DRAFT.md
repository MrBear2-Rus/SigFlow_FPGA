> 2026-10-31 更新：用户已确认 D03/D05/D07/D08。已实现的本地基线见
> [contracts/run-v1/README.md](contracts/run-v1/README.md) 及对应 schema/fixtures。
> 下文保留最初评审内容；与本地基线不一致时以该基线及 task.md 的最新状态为准。
> P2 已实现 run 入口与单任务 Runtime；真实 provider 和 Node 实际联调尚未完成。
> 当前实现细节及接入说明见 [P2_HANDOFF.md](P2_HANDOFF.md)。

# Node ↔ C++ run 协议草案

状态：协作评审草案，尚未接入 CLI。2026-10-31，协作者确认沿用旧契约。
依据：旧 `include/eda/api/jobs.hpp`、`core/src/eda-core/JobService.cpp`、
各 InnerPlugin provider；迁入后以 web 内契约及 provider 为准，不产生运行依赖。

## 输入与边界

沿用 README 的 `run --job-type --work-dir --job-id --project --params`。
`--params <file|->` 只装载旧 JobRequest.params 对象，不混入 JobRequest 外层字段。
例如 synth 参数：

```json
{"top_module":"top","source_files":["/snapshot/top.sv"],"strategy":"baseline","yosys_family":"gw1n"}
```

保持旧 provider 参数名，不将 top_module 改成 topModule。CLI 不接受任意 shell
命令替代 job-type；由静态分发表选 provider。文件中的参数必须为 JSON 对象。
Node 必须传不可变工程输入路径；C++ 输入只读，所有输出限制在 work-dir 中。
保留旧 output_json 等字段，但必须校验其落在 work-dir 内，不能沿用任意写路径。

映射建议：

| 来源 | JobRequest 字段 | 规则 |
|---|---|---|
| --job-type | jobType | synth/pnr/pack/flash/sim.build/sim.run 白名单 |
| 静态分发表 | pluginId | 使用原 provider 标识，不从不可信参数选择插件 |
| Node 工程标识 | projectId | project 路径与 ID 分开；新增 --project-id，不能把路径冒充 ID |
| --params | params | 保留原工具参数结构 |
| --timeout-sec | timeoutSec | 非负整数；省略由 Node 下发默认值 |
| Node 可信来源数据 | metadata | 独立于 params；首版默认空对象 |
| Node 明确确认 | requireConfirm | 建议新增 --confirm；flash 无确认则拒绝 |

project-id 与 confirm 是 README 未列出的补充参数，需主 contributor 审阅后再冻结。

## 落盘

采用 README 目录约定，报告名统一 `reports/report.json`。
旧程序用 `reports/job-report.json`，Node Gateway 适配层必须知道这一差异；
不额外创建第二份权威报告。

manifest 沿用旧 eda.job.v1 的 snake_case 序列化，而非 README 的概念性 camelCase：
`schema_version/job_id/retry_of/job_type/plugin_id/project_id/state/created_at/updated_at/exit_code/parameters/metadata/transitions`。
request.json 保存执行参数快照和调用标识，定义独立的版本字段后再实现。
JSON 文件采用同目录临时文件替换，失败时禁止先报告成功。

report 保留旧 eda.jobreport.v1：

```json
{
  "schema_version":"eda.jobreport.v1",
  "job_id":"job-001",
  "job_type":"synth",
  "plugin_id":"<original-provider-id>",
  "state":"Succeeded",
  "exit_code":0,
  "artifacts":[],
  "metrics":{},
  "diagnostics":[]
}
```

artifact 字段保留 `id/path/schema/sha256/role`。建议 path 改为 job-dir 相对路径，
Node 下载接口按 job-dir 解析；此路径语义变化必须在 Node 与 C++ 同步采用。
metrics/diagnostics 的具体结构沿用各 provider 实际输出，不凭示例固定所有类型。

## JSONL 和退出码

事件按 README 使用 camelCase（例如 result.exitCode/jobId），与旧落盘字段分开。
stdout 只能是 JSONL；工具原始 stdout/stderr 转为 log 事件，不能直通。

```jsonl
{"type":"log","level":"info","line":"Starting synthesis"}
{"type":"progress","percent":20,"status":"Running"}
{"type":"artifact","id":"netlist","path":"artifacts/top.json","role":"netlist","sha256":"<hex>"}
{"type":"result","state":"Succeeded","exitCode":0,"jobId":"job-001"}
```

result 只发一次，且在最终报告落盘后发送。provider 的工具退出码保留在报告及
result.exitCode；CLI 进程退出码遵循 README：0 成功、2 参数错误、3 未迁移、4 执行失败。
因此工具 exit 17 时 result.exitCode 可为 17，但 CLI 退出码是 4。

C++ 只执行 Running/ValidatingArtifact/终态，不维护 Queued 或重试队列。
超时为 TimedOut，取消为 Cancelled，其他执行错误为 Failed。
Node 不能只依赖最后一行：崩溃/强杀时无 result，须结合退出信号与报告恢复。
SIGTERM 通过安全的信号通知机制触发 IProcessHost::Cancel，禁止在信号处理器里
写 JSON、加锁或调用复杂 C++ 方法；工具自建进程组需显式清理。

## 首次联调验收

1. 无真实 FPGA 工具也能用测试 provider 验证：参数 → 工作目录 → JSONL → 报告。
2. 验证工具缺失、参数错误、非零退出、超时、取消和写报告失败。
3. 验证无跨工程输出、日志中引号/换行可解析、终态事件唯一。
4. 再用真实 Yosys 验证 top.json、sha256、报告与 Node 展示一致。

本草案未定义 wave serve、结构树、诊断等其他命令的协议，另行扩展。
