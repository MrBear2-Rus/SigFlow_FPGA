# 阶段 9：只读报告联调命令

日期：2026-10-11。交付为本地联调入口，真实 C++ Gateway 验收仍待执行。

## 使用方式

工作目录为 sigflowAgent，使用现有 .venv，不要求激活 Conda 或安装 UCAgent。
可信宿主需通过进程环境注入以下配置；入口不会自动读取 .env：

| 环境变量 | 含义 |
|---|---|
| SIGFLOW_GATEWAY_PORT | 本机 Gateway 端口 |
| SIGFLOW_GATEWAY_TOKEN | Agent Bearer token，不写入命令参数或文档 |
| SIGFLOW_GATEWAY_INSTANCE | 期望实例 ID |
| SIGFLOW_JOB_ID | 已存在的 Job ID |
| SIGFLOW_PROJECT_ID | 期望项目 ID |
| SIGFLOW_REVISION | 期望源码版本 |
| SIGFLOW_SNAPSHOT_ID | 期望快照 ID |

```powershell
.\.venv\Scripts\python.exe -m sigflow_edu_agent.clients.probe --help
.\.venv\Scripts\python.exe -m sigflow_edu_agent.clients.probe --report
$LASTEXITCODE
```

无参数运行仍查询 health 和 capabilities；--report 只经过 health 与绑定 Job 的 report 路由。
网络边界、超时、响应上限和错误码沿用[阶段 8](stage-8-gateway-report.md)。
入口不调用 DeepSeek、不创建 Job、不写文件、不自动重试。

## 输出契约

成功时 stdout 为单个 JSON 对象：instance_id、job_id、project_id、revision、snapshot_id、
state、completeness、diagnostic_count、artifact_count、evidence_validated。
缺失 diagnostics/artifacts 按零计数，计数不表示诊断或 artifact 内容已验证。
evidence_validated 固定为 false；报告读取不执行可信锚点验证。
不输出报告自由文本、路径、诊断内容或凭据。

退出码 0 表示绑定报告读取成功，包括 Running/partial 报告；不能据此认定 Job 成功、
设计无缺陷、证据有效或获得执行权限。配置/请求失败返回 2，stderr 为固定错误码，
stdout 为空。缺失绑定为 INVALID_ARGUMENT，版本不符为 STALE_REVISION，报告不存在为 NOT_FOUND。
参数解析错误由 argparse 显示用法并返回 2。

## 验证

新增 8 项测试，包括独立子进程连接本地 HTTP fixture；验证仅发送 GET、health 不带 token、
报告请求携带 token、无效绑定不连接、版本过期拒绝及服务器错误正文不泄漏。
初次测试观察到入口缺少参数接口；实现后全量验证结果如下：

- 配置真实 UCAgent 源码路径：148 项全部通过。
- Python -S 核心隔离：142 项通过，6 项可选集成跳过。

```powershell
.\.venv\Scripts\python.exe -m unittest discover -s tests -p test_gateway_probe.py -v
.\.venv\Scripts\python.exe -S -m unittest discover -s tests -q
```

本阶段未连接真实 C++ 服务、未调用真实 DeepSeek API。本地 HTTP fixture 不计入真实联调验收。

## 再规划

只读入口已可交接给 U1/S1/S2。下一步由宿主注入实际实例与 Job 绑定，运行上述命令并记录
输出与退出码；随后由 S2 提供可信 EvidenceAnchor 目录，接入 GatewayReportSource。
不从报告文本自动推导锚点。审批摘要一致性与 L4 双确认继续等待跨组协议验收。
