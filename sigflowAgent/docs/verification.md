# 第一阶段验证记录

日期：2026-09-29。环境：Windows / PowerShell / Python 3.13.5。

## 已执行

在 sigflowAgent 目录运行：

```powershell
python -m unittest discover -s tests -q
python -m compileall -q sigflow_edu_agent tests
python -m sigflow_edu_agent --demo
```

- 33 项测试通过；compileall 退出码 0。
- CLI 测试以独立子进程验证离线 demo、缺 DeepSeek 配置的明确错误，以及 next/finish 交互。
- demo：解释 Completed；两次更多提示后 L3/WaitingStudent；未批准 WaitingApproval；失败验证 Failed；成功工具执行 Completed，并明确未证明功能正确。
- DeepSeek 测试保留真实请求构造/解析逻辑，仅替换 HTTPSConnection 网络边界；覆盖 JSON、鉴权、限流、超时、空/截断输出、字节预算和规则降级。
- 独立代码审查发现坏类型/坏 Unicode 证据可能抛错后保持 E6 可执行。先增加失败回归，再使证据检查对这些输入返回 False。复核确认转入 Failed，再推进不会增加执行次数；独立重跑 33 项测试通过。

## 尚未验证

- 未使用真实 DeepSeek API Key 发起请求，未验证账户模型可用性、网络连通性或真实教学质量。
- 未执行真实 EDA Job、未接入 SigFlow Gateway/GUI、未跑真实 UCAgent stage/UCTool/Checker。
- 未验 Linux、Python 3.11 运行环境、持久化恢复、多线程服务或教师评测。

此记录只证明第一阶段领域核心与离线适配边界，不代表周期四 Gate 通过。
