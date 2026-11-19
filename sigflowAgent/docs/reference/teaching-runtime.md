# U2 教学主循环

教学逻辑由 `TeachingLoop` 维护。宿主调度工作线程执行 `run_until_pause()`，状态进入等待后，由显式学生动作、可信审批或恢复入口继续推进。

```text
E0 初始化 → E1 取证 → E2 检查 → E3 教学路径 → E4 发布
  explain → 完成
  diagnose → E5 等待学生 → 下一提示 → E1
  verify → E5 等待审批 → E6 验证 → E7 反馈
```

默认预算为 32 次阶段步和 4 次模型调用。领域循环校验动作版本；在组合服务的 HTTP 边界，客户端提交状态响应的 `state_version` 作为请求的 `expected_version`。正式宿主管理异步推进、SQLite 持久状态及恢复，普通对话输入不能替代动作或工具审批。

- [领域循环](../../sigflow_teaching_core/loop.py)定义步骤、教学状态、预算和领域动作。
- [宿主运行时](../../sigflow_teaching_core/host/runtime.py)管理工作线程、提交、恢复与挂起。
- [教学策略](policy-v1.md)定义分级提示和权限边界。
- [GUI 接入手册](../interfaces/u1-u2-teaching.md)定义调用顺序、版本校验与只读投影。
