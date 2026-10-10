# SigFlow Edu sidecar bootstrap v1

> 2026-10-10 宿主实现：bootstrap 是 <=64 KiB 的紧凑单行 UTF-8 JSON，stdout 只允许 ready 一行；收到 ready 后必须校验标准 health/instance/version 才发布业务连接。重启轮换 UI→Agent token 并失效旧代次；Stop 有界 shutdown 后终止当前子进程。data_directory 改为稳定用户数据位置，以保存最小学习数据/恢复检查点，不保存会话或自动继承授权。完整边界见 [实现与验收](10.10-Agent接口调整实现与验收.md)。

这是 SigFlow 桌面端与 Python `sigflow_edu_agent` 的启动边界。它不替代
HTTP/OpenAPI 契约；它只解决“哪一个本地 Python 进程可以代表当前 IDE 实例”的问题。

## 宿主行为

设置绝对路径环境变量 `SIGFLOW_EDU_AGENT_EXECUTABLE` 后，SigFlow 以：

```text
<configured executable> --bootstrap-stdin
```

启动 sidecar。命令行、URL、工程文件和普通日志中不包含 token、nonce 或模型 Key。
启动后的标准输入第一行是 UTF-8 JSON：

```json
{
  "type": "sigflow-bootstrap",
  "protocol": "edu.api.v1",
  "instance_id": "inst-...",
  "nonce": "256-bit hexadecimal random value",
  "gateway_url": "http://127.0.0.1:<port>",
  "gateway_token": "Agent-to-EDA bearer token",
  "ui_token": "UI-to-Agent bearer token",
  "data_directory": "per-instance user-data directory"
}
```

该行经子进程 stdin 仅发送一次。sidecar 不得回显、持久化或记录其中任一 token；其工作目录和数据目录都不是工程根目录。

## sidecar readiness

sidecar 在绑定 loopback 随机端口并完成初始化后，向 stdout 写**一行且仅一行**：

```json
{
  "type": "ready",
  "protocol": "edu.api.v1",
  "nonce": "same value as bootstrap",
  "port": 18423,
  "agent_version": "package-version"
}
```

SigFlow 在 15 秒内验证 nonce、协议、端口和非空版本。之后它以 `Authorization: Bearer <ui_token>`
轮询 `GET http://127.0.0.1:<port>/api/v1/health`；响应必须为 200，且 JSON 根或 `data` 对象中的
`protocol` 必须等于 `edu.api.v1`。

有效 ready 之前的 stdout 只可作为 sidecar 自身日志，不能改变 SigFlow 状态。stderr 不会被转发到
主界面或保存为含敏感数据的日志。

## 退出和恢复

ready 超时、异常退出或连续两次 health 失败时，SigFlow 最多自动重启 3 次，延迟为 1、2、4 秒。
每一次启动均生成新的 nonce 和 UI→Agent token；Agent→EDA token 目前随 IDE Gateway 生命周期创建，
sidecar 重启不会自动恢复或执行旧 grant。Gateway token 的热轮换需与 U1 的重认证流程一并联调后再启用。
到达上限后仅禁用 Agent 功能，编辑器、旧有仿真和综合仍照常可用。
