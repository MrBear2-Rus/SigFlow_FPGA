# 阶段 29：操作级学习接收适配

`host.memory_adapter.MemoryAdapter(receiver_factory, scope_allowed)` 可直接作为 Runtime 的 `teaching_memory` 参数。工厂必须返回可信 context manager；连接的创建、使用和退出都在当前操作线程完成，不能闭包共享现成 SQLite 连接。事实复用既有 `validate_fact`，回执只接受当前 event_id 与布尔 accepted；历史严格匹配 learner/project/concepts，并限制字节和控制字符。入参、回调参数、接收端结果均深复制。

`scope_allowed(scope)` 必须由可信组合层判断授权和撤销，只接受显式 `True`。事实 scope 包含完整工程/会话/版本/学习者/policy 绑定；历史沿用既有 learner/project/concepts 契约，不虚构历史接口未提供的 revision。每次外部调用前后及返回围栏重新检查，`close()` 不等待慢接收器并拒绝后续返回。外部异常统一为 `memory_unavailable`，不输出消息或路径。

实际验证：`test_memory_adapter.py` 5 项通过。先验证模块缺失的 5 项失败，再实现并验证通过；用真实 Runtime、DurableStore/outbox 和 history worker，加合成接收契约与默认线程受限 SQLite 连接验证线程生命周期、复制、错范围、错回执、撤销、关闭和异常脱敏。现有 `test_teaching_memory.py` 在阶段 30 联合定向回归中复验：总计 50 项，49 通过，1 项 Windows symlink 权限不足跳过（18.845 秒），无失败。

这不是 UA-10 数据库实现，也未接上实际 U3 接收端。U3 仍负责持久化 event_id 去重、删除 tombstone 及写入/删除事务竞争。调用后撤销只阻止 U2 交付回执/历史，无法撤回接收端已经提交的副作用；实际接收器必须在自己的事务内校验撤销。接收工厂和授权回调由后续可信宿主组合提供，不从 HTTP 或模型输入产生。
