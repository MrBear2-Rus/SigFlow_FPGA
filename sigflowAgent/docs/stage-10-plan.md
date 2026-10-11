# 阶段 10：宿主会话组装与 occurrence 去重

目标：为 UA-05 提供可接入宿主的只读会话工厂，将已完成的 GatewayReportSource、
DeepSeekProvider 和 TeachingSession 组合为单一入口。

设计：采用 runtime/gateway_session.py 组合既有模块，不修改共享 HTTP 契约或替换 U1 服务。
相比继续手写组装脚本或增加新的服务进程，工厂便于复用和测试，且不引入生命周期竞争。
绑定包含实例、learner/session、issue、项目/版本/快照、Job 和诊断，全部由可信宿主传入。
仅支持 explain/diagnose；verify 仍由原有受控执行端口与跨组授权协议承接。
模式默认 deepseek，rule 显式选择；构造不联网，advance 才按状态机取证/调用模型。
单会话固定一个 Job/诊断，仅在首次有效取证时记录 occurrence；同绑定内容变化拒绝继续使用。
记录只保存绑定、证据 ID 与摘要哈希，不存报告正文。不实现跨进程恢复或学生错误次数统计。

- [x] 先写失败测试：范围不符、无效模式、DeepSeek 缺配置、规则流程、重复刷新、证据变化。
- [x] 实现工厂与去重源，复用既有锁和版本校验。
- [x] 验证无效证据不调用模型、暂停不自转、跨实例拒绝和只读约束。
- [x] 全量回归，产出阶段文档并更新 task.md。

所有变更位于 sigflowAgent；不提交、不推送。真实联网、授权核销及教师验收不以替身测试代替。
