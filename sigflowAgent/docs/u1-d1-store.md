# D1 持久状态与预算开发记录

日期：2026-10-15。实现入口：`sigflow_edu_agent/host/store.py`。

## 交付范围

新增纯标准库 SQLite `DurableStore(path, instance_id)`。数据库和持久 `.lock` 文件由宿主配置路径决定；Windows 使用 `msvcrt.locking`，POSIX 使用 `flock`，拒绝第二个持有者。锁文件不删除，不用 PID 探测判断 Windows 进程存活。数据库实例绑定不匹配时拒绝打开，避免混用不同实例的数据。

同一实例内的线程通过 `RLock` 和 `BEGIN IMMEDIATE` 串行事务访问。状态、检查点、动作结果和事件在同一提交中更新；SQLite 故障回滚并返回 `storage_error`，不会将候选结果作为成功返回。使用完整同步和 DELETE journal，数据库文件另设 65,536 页上限（通常约 256 MiB）。

## D2 接口

| 方法 | 输入与结果 |
| --- | --- |
| `create(session_id, binding, request_sha256, mode, intent, state, *, initial_action=None)` | 创建固定范围会话；可用 `(action_id, fingerprint)` 同事务预留初始动作。返回 `None`；重复 ID 为 `session_exists` |
| `get(session_id)` | 返回新解码的会话字典，字段见下文 |
| `list_sessions()` | 按创建顺序返回完整会话字典列表 |
| `claim(session_id, action_id, fingerprint, expected_version)` | 新动作持久预留返回 `None`；同指纹重放返回 `{status, result}` |
| `commit(session_id, action_id, state, checkpoint, result)` | 将预留动作、进展后的状态、检查点、结果和事件原子提交，返回 `None` |
| `action(session_id, action_id)` | 返回 `{status: pending/committed/interrupted, result: dict/None}` |
| `reserve_model(session_id, maximum)` | 模型调用前原子增加已用额度，返回新的计数；额度耗尽为 `budget_exceeded` |
| `model_used(session_id)` | 返回持久额度计数 |
| `request_cancel(session_id)` | 独立于动作预留设置持久取消标记，返回 `None` |
| `cancel_session(session_id)` | 特权教学取消：清空卡片/检查点、增加版本、设置标记、中断 pending 并写事件。独立于客户端动作键及容量，不声明 Job 终止 |
| `cancel_requested(session_id)` | 返回布尔取消标记 |
| `events(session_id, after, limit=100)` | 返回 `{events, high_watermark}`，按序号升序且严格晚于 `after` |
| `close()` | 关闭数据库并释放 OS 锁；可重复调用，保留锁文件 |

会话字典包含 `session_id`、`binding`、`request_sha256`、`mode`、`intent`、`state`、`checkpoint`、`model_used` 和 `cancel_requested`。`binding` 接受完整 `SessionBinding` 字典并验证实例与会话 ID；`state` 必須有合法状态、`cards` 列表和规范十进制字符串 `state_version`。`expected_version` 是非布尔整数，初始 `0` 合法。JSON 会将教学卡片的 tuple 转为数组；读取结果与输入对象没有共享可变引用。

动作键作用域为会话。不同指纹复用同一键返回 `action_conflict`；相同键优先重放原结果，即使当前版本已变化。不同键不能越过同一会话尚未完成的动作，返回 `session_busy`；新动作的旧版本返回 `state_conflict`。提交要求动作仍为 pending，且状态版本严格增加。

取消标记在提交事务内再次校验。标记设置之后只允许提交 `Cancelled`、`Failed` 或 `BudgetExceeded`；其他候选状态（含 `Completed`）返回 `cancel_requested` 并保留动作预留。`cancel_session` 已中断的动作不能提交任何候选，D2 只发布持久取消投影。模型预算预留同时拒绝已取消会话。

事件仅包含 `sequence`、`action_id`（恢复事件为 `None`）、字符串 `state_version`、`status`。提交和首次恢复隔离生成事件；保留最后 1,000 个事件，裁剪后高水位仍单调增加。

## 重启与数据边界

首次打开数据库时，将 pending 动作标为 interrupted，不自动重跑。非终态且尚未处于 `RecoveryRequired` 的会话清空可发布卡片，状态变为 `RecoveryRequired`，版本增加一次（版本空间允许时）。连续重启保持该版本；恢复由 D2 传入持久版本作为可信下界。`Completed`、`Cancelled`、`Failed`、`BudgetExceeded` 保持终态。此过程不覆盖原检查点，也不改写已提交动作的历史重放结果；对外重放时由 Runtime 执行恢复和取消隔离。

检查点保存独立 SHA-256 摘要，启动和返回会话或动作前校验；不匹配为 `checkpoint_corrupt`。摘要用于检测意外损坏，不能充当签名或授权。重启的只读核心恢复和重新取证由 D2 负责。

数据限制为 32 个会话、每会话 256 个动作、每会话 1,000 个保留事件、单个检查点 128 KiB、单个状态或结果 JSON 128 KiB、绑定 JSON 16 KiB。会话与动作达到上限后拒绝新增，历史幂等记录不悄悄淘汰。预算上限输入为 0–100。标识和指纹均有长度界限；非法 JSON 值、NaN、非规范版本、敏感凭据和请求字段均被拒绝。

学生原问题只由宿主计算摘要后传入 `create`，不存储请求正文。`cards[].question` 是已批准的教学卡片文本，可以存储；顶层学生 `question` 以及 `request`、`request_body`、grant、token、API key、receipt 等字段被拒绝。检查点接口接收宿主生成的只读文本，限制字节数并对可解码 JSON 做同样字段检查；任意不透明文本的内容来源仍由可信宿主管理。

稳定异常类型为 `StoreError(ValueError)`，`str(error)` 与 `error.code` 均是小写错误码。主要错误包括 `invalid_input`、`binding_conflict`、`instance_conflict`、`store_locked`、`store_closed`、`session_not_found`、`action_not_found`、`session_exists`、`session_limit`、`action_limit`、`action_conflict`、`session_busy`、`state_conflict`、`cancel_requested`、`budget_exceeded`、`checkpoint_too_large`、`checkpoint_corrupt`、`state_corrupt`、`storage_error`。原始数据库和文件路径诊断不进入错误响应。

## 验证记录

定向命令（工作目录 `sigflowAgent`）：

```powershell
.\.venv\Scripts\python.exe -m unittest discover -s tests -p test_host_store.py
```

先运行 14 个首轮用例，全部因 D1 模块缺失而失败；实现后通过。新增 tuple 卡片、取消原子门控、非法状态、额度损坏、事件裁剪与启动损坏检查；观察取消与输入用例失败后修复。连续重启用例先观察版本从 6 错升至 7，再实现幂等恢复。最终 20 个用例通过。

测试使用测试自身创建的临时目录，涵盖真实 Windows 同进程/子进程锁争用、动作重放与冲突、版本 CAS、预算多线程竞争与重启持久、取消标记、终态重启、检查点篡改、事务中真实 SQLite trigger 故障回滚和容量拒绝。事件裁剪在测试内缩小保留上限以触发同一分支，避免上千次磁盘重启。未新增第三方运行依赖，未提交或推送。

此阶段不宣称已验收 Linux 文件锁、真实 Gateway、真实 DeepSeek、完整 L4 或磁盘断电恢复；后续 D2–D5 分别覆盖教学承载、HTTP、受控验证和进程联调。
