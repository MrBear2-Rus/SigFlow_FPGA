# 阶段 35：本地连接故障与回归收尾

日期：2026-10-22。基线 deb6bd9；从保存点 875fb732 恢复，保存点及此前 stash 均保留。旧 GUI demo 未恢复，本轮不提交或推送。

## 故障定位

此前全量回归在本机 HTTP 连接阶段出现 WinError 10048，独立于 U1/U2 的标准库探针又在绑定监听端口时遇到 WinError 10055；另一次探针完成 39 次请求后在客户端连接阶段返回 10048。错误并非只出现在教学宿主或 Gateway 业务层。

Microsoft 的 [Winsock 错误码说明](https://learn.microsoft.com/en-us/windows/win32/winsock/windows-sockets-error-codes-2)将 10048 定义为地址已使用，10055 定义为缺少套接字缓冲/队列资源。[端口耗尽排查说明](https://learn.microsoft.com/troubleshoot/windows-client/networking/tcp-ip-port-exhaustion-troubleshooting)提供了对应排查方法。上述错误可能有多种原因；本次只读检查显示 IPv4 动态端口范围为 49152–65535、16384 个端口，观察到的 TCP 连接数远低于该范围。现有证据不足以确认动态端口耗尽、具体驱动或某个外部进程为根因，不把推测写成结论。

重新恢复本轮工作后，独立 1000 次回环请求成功，随后两套回归串行执行。最终另以 3000 次全新标准库 HTTP 连接验证环境，结果归档于[回环记录](loopback-stage35.json)。探针没有导入 U1/U2，没有自动重试失败请求，也没有发送业务请求或读取 Key。当前环境不再复现原连接故障；不能据此保证未来所有系统负载下都不会出现同类故障。

## 已修正的仓库问题

- 可选 U2 测试先检查包及 Python 版本，基础 U1 安装缺少 U2 时明确跳过组合测试；已安装包内部导入错误仍暴露。版本分支模拟不计作真实 Python 3.9/3.10 验证。
- U1 teaching 与 U2 acceptance 测试自行创建其临时父目录，不依赖此前测试留下的阶段目录。
- `agent/bin/.gitattributes` 对 POSIX 启动器指定 `text eol=lf`，脚本原始字节与 HEAD 完全一致。一次 stash apply 在恢复尚未跟踪的属性文件前已写出 CRLF，导致两项检查再次失败；重新规范换行后完整回归通过。后续提交应包含该属性文件，防止其他 Windows 检出再次转换脚本。执行模式和启动逻辑均未改变。

未调整注册表、动态端口范围、防火墙、TIME_WAIT、SO_LINGER 或全局 Git 配置；没有终止其他应用进程，没有添加生产请求重试，没有额外跳过失败测试。首次提权诊断因自动审批超时未执行，允许的一次重试成功；该审批问题没有计作测试通过。

## 验收记录

当前基线 deb6bd9 的完整回归：U1 326 项、1 条平台跳过；U2 719 项、37 条跳过记录；两套失败/错误均为 0。U2 逐项成功 684，单项跳过 35，另有 2 条类级跳过记录。开发评测 40/40；独立离线安装/名称隔离 4/4；真实当前工程闭环 3 Job、0 次模型调用。

U1 唯一平台跳过为 Windows 不支持可捕获 SIGTERM。U2 的课程原文快照、历史 UCAgent、Windows symlink 权限和离线 wheel 工具条件等跳过逐项列在[机器验证](verification-stage35.json)中；wheel 安装在已有离线构建工具的解释器中另行验证 4/4。跳过不计为通过。

开发集使用规则/Mock，不是团队冻结模型批次。真实三 Job 报告是本阶段此前已执行的正式 U1→U2→Core/Yosys/Verilator 场景，其运行时源码保持一致，本次未重建 C++ 或再次消费模型额度。该场景覆盖持久关联、恢复、提示重放、审批后继、精确波形预测、版本隔离与取消；未操作原生 GUI。源码/二进制摘要及历史 JSON 完整性见机器验证。

## 复现与后续处理

在仓库根目录配置 U1 和 U2 源码导入路径后，分开执行完整测试，前一套结束后再启动下一套。测试环境应使用缺省规则/Mock 配置，未显式开启真实模型评测时不提供批次输入。

```powershell
$previousPythonPath = $env:PYTHONPATH
try {
    $env:PYTHONPATH = "$PWD\agent\src;$PWD\agent;$PWD\sigflowAgent"
    .\sigflowAgent\.venv\Scripts\python.exe -m unittest discover -s agent/tests -t agent -v
    if ($LASTEXITCODE -ne 0) { throw 'U1 regression failed; inspect output before continuing' }

    $env:PYTHONPATH = "$PWD\sigflowAgent"
    .\sigflowAgent\.venv\Scripts\python.exe -m unittest discover -s sigflowAgent/tests -v
    if ($LASTEXITCODE -ne 0) { throw 'U2 regression failed; inspect output' }
} finally {
    $env:PYTHONPATH = $previousPythonPath
}
```

若 10048/10055 再次出现，先保存失败栈和当前连接快照，不将其改为通过或无限重试。以下只读命令可辅助判断连接与端口情况：

```powershell
netstat -ano -p tcp
netsh int ipv4 show dynamicport tcp
netsh int ipv4 show excludedportrange protocol=tcp
```

当前输入下 U2 自有实现及必要 U1 接线已完成本阶段验证。真实 UA-10、L4 审阅材料/教师权限、S2 新检测事实、冻结模型批次/双教师、正式 GUI 和目标平台仍按[交付核对表](u2-completion-status.md)分别验收；本次收尾不将这些外部条件标记为完成。
