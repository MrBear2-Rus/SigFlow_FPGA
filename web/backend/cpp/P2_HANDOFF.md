# P2 单任务 Runtime 交接

更新：2026-11-01。C++ 本地闭环已实现；Node 应用实际接入和主 contributor 集成验收单列。
任务依据为 task.md 的 P2；用户已确认 D09 采用短生命周期任务守护进程。

## 1. 调用约定

```sh
sigflow-tool run --job-type synth \
  --project /srv/projects/project-001 --project-id project-001 \
  --job-id job-001 \
  --work-dir /srv/projects/project-001/.sigflow/agent/jobs/synth/job-001 \
  --params /srv/requests/job-001.json --timeout-sec 60 --max-output-bytes 8388608
```

`--params -` 接收一个完整 JSON 对象，Node 写完必须关闭 stdin；不是 JSONL 多请求服务。
参数文件同样只接受一个完整对象。输入上限 2 MiB；未知/重复选项、缺值、负数、溢出、
非法 ID 和非对象 JSON 返回 CLI 2。ID 使用 1–128 位 ASCII 字母/数字/点/下划线/短横线，
但不允许单独的 . 或 ..。工程与工作目录可为相对路径，路径中不接受 .. 段或符号链接。

Node 必须预先创建**空** Job 目录；目录必须严格匹配 project/jobType/jobId 的约定。
Node 的排队事实保存在 Node 侧，不要预写 manifest.json、request.json 或工具目录。
Runtime 用 .runtime-claim 原子认领；它保留在任务目录中，重复调用不能覆盖已有任务。
重试应由 Node 分配新 jobId，不复用旧目录。Node 独占工程授权，不把任意用户路径直接传给 CLI。

生产静态工厂已接入 synth provider；pnr/pack/flash/sim.build/sim.run 仍返回 3。
真实 Yosys 和 Node 集成尚未在当前环境验收。
测试专用 sigflow-tool-fixture 才绑定 synth fixture，既不进入生产目标，也不安装。

## 2. C++ provider 接入方式

- 保留旧 IJobProvider / JobContext；startJob 同步执行，返回前结束自建后台活动。
- 所有工具经 ctx.processHost().Run，所有日志经 ctx.log；不能绕开 Runtime 直接写 stdout。
- ExecutionFor(ctx) 读取当前项目/工作目录、工具根、资源目录、限额等执行设置。
- CheckedInput(ctx,path) 只接受工程目录内文件，或显式选定 data-dir 下的绝对文件路径；
  拒绝不存在文件、目录、.. 和符号链接。外部源码快照需放在授权工程内；其他授权根另行定义。
- CheckedPath(ctx.jobDir(),path,false) 验证临时输出；登记产物必须落在 artifacts/ 下。
  验证后才写文件，不能先写到任意路径再靠 registerArtifact 拒绝。
- ResolveExecutionTool(ctx,query) 将显式工具根与默认 runtime 分开，遵循：
  configuredPath → 显式 --tool-root → 每工具环境变量 → 默认 runtime → PATH。
  结果为绝对可执行路径，避免工具 cwd 切到 Job 目录后破坏相对路径。
- request.metadata.tool_root_source 由 CLI 写 argument/environment/default，用于记录配置来源；
  不从用户 params 接收宿主 metadata。request 的整体 schema 未变。
- provider 负责工具特有参数、必要文件/输出格式和副作用校验；Runtime 不把进程退出 0
  当成某种工具产物格式已经正确。长时间纯计算代码需主动检查 ctx.cancelled()。

工程/Job 目录必须受 Node 权限管理，不能允许其他租户并发替换目录。路径检查拒绝已存在的
符号链接，但不是 OS 沙箱；对恶意并发文件操作的隔离由权限与部署沙箱承担。

## 3. 状态与产物

Runtime 写 request.json，进入 Running；provider 完成后进入 ValidatingArtifact，
重新校验已登记文件的 SHA256，再写 reports/report.json 与终态 manifest，最后发 result。
JSON 使用同目录独占临时文件、fsync、rename、父目录 fsync；报告失败不会先写成功 manifest
或发成功 result。多文件不是一个数据库事务，Node 仍需结合进程退出及完整报告判断。

Artifact.path 是相对 Job 目录路径，必须为 artifacts/ 下的普通文件。登记时校验摘要，
终态前复核，重复 id、缺文件、符号链接、越界或摘要变化导致 Failed。
artifact 事件在最终验证前为候选事件，Node 不应仅凭该事件宣告任务成功或发布产物。
失败报告不发布 artifacts；现场文件保留供诊断，不能据目录里有文件认定有效结果。

CLI 与工具退出码不同：CLI 0/2/3/4；真实工具状态保留在 report.exit_code/result.exitCode；
未获得真实工具退出状态时为 -1。无进程的纯 provider 可成功返回但 exit_code=-1。
SIGTERM/SIGINT 正常清理后会重新以原信号退出，报告和 result 的 state 为 Cancelled。
Node 应同时处理 report、result 和 signal；不要将 signal 一律覆盖为普通 Failed。

## 4. 限额、流与取消

- --timeout-sec 是整个 Job 的执行预算，包括多次工具调用与产物验证；不在每步重置。
  未传/0 不设置此预算；Node 可下发部署默认值。单次 ProcessSpec 的更短超时仍可生效。
- --max-output-bytes 对当前 Job 的原始日志累计保留量生效；超额继续排空工具管道，
  不因日志过多强杀工具。report.metrics 中追加 output_truncated=true。
- logs/tool.log 保留接收的原始字节；stdout JSONL 对无效 UTF-8 使用替换字符。
  log.line 表示有限数据片段，可能包含换行；Node 不要假设恰好是一条工具物理行。
- stdout 断开会触发取消；持续无法写入超过 2 秒视为通道失效并停止执行。
  通道失败后不再尝试追加事件，消费者应丢弃可能存在的最后一个不完整 JSON 行。
- SIGTERM/SIGINT 的 handler 只设置标志；监控线程取消工具组、wait/reap，并尽力落报告。
  参数尚未读完时收到取消也能退出，不需要等待 stdin EOF。
- 每次实际工具执行前建立独立 guardian，再释放启动门。CLI 意外退出/SIGKILL 导致
  lifetime pipe EOF，guardian 清理工具组后退出。工具通过 PDEATHSIG 补充直接父进程死亡保护。
- 正常结束会回收 guardian。SIGKILL 无法落盘终态，Node 扫描残留 Running 进行恢复；
  硬杀后的孤儿退出进程由系统 init 回收，回归检查其不再运行。
- guardian 没有 RPC、队列、用户会话或跨调用状态；不替代 Node 的作业管理。

## 5. 验证与 Node 待接入清单

本地测试运行同一份 CLI/Runtime，测试 fixture 用编译宏隔离；使用真实 /bin/sh、sleep 等
进程验证 timeout/SIGTERM/SIGKILL/EPIPE，不能据此宣称 Yosys/板卡或实际 Node 服务已联调。

Node 接入时需要：
1. 使用 detached 进程与 pipes；完成参数写入后关闭 stdin。
2. 使用空目录，遵循 --project-id、相对产物路径和 reports/report.json。
3. 解析有界 JSONL，不解析自由文本；处理候选 artifact、无 result 和不完整尾行。
4. SIGTERM 优先，给清理/落盘留出时间，再升级 SIGKILL；退出信号与报告一起判断。
5. Node 重启扫描残留目录，自己管理恢复、队列、重试和设备租约。
6. 以生产工厂未迁入返回 3 区分“不支持”与一次工具执行失败。

本次不修改 Node/前端/Python，不执行 Git 提交或推送。真实 provider 的接入从 P3 继续。
