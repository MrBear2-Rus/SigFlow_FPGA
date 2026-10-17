# D8 状态与事件重同步兼容记录

日期：2026-10-15。范围为 U2 直接依赖的动态证据供应、验证只读观察和状态/事件重同步；保留 U1 独立打包、桌面 UI 与团队发布工作。

## 存储接口

`DurableStore.get(session_id)` 返回的会话字典增加整数 `event_high_watermark`，初始值为 `0`。现有字段继续返回。`list_sessions()` 使用相同解码入口，也提供该字段。

高水位直接读取持有状态的同一 SQLite 行中的 `event_seq`，沿用 `RLock` 和 `BEGIN IMMEDIATE` 事务，不在读取状态后另开事务查询事件。状态版本与事件序号是不同计数：一次状态提交可以跨越多个版本，但只产生一个事件。动作预留、模型额度预留和取消标记本身不产生状态事件；成功提交、首次启动隔离和特权教学取消沿用原事件写入机制。

`events(session_id, after, limit=100)` 继续返回 `{events, high_watermark}`，事件按序号升序、严格晚于 `after`，分页不降低会话的高水位。同一事务内新增以下检查：

| 游标条件 | 结果 |
| --- | --- |
| `after > event_seq` | `StoreError('cursor_stale')` |
| `after < 首个保留事件序号 - 1` | `StoreError('cursor_expired')` |
| `after == 首个保留事件序号 - 1` | 正常返回首个保留事件及后续页 |
| `after == event_seq` | 返回空事件列表及当前高水位 |
| 新会话且 `after == 0` | 返回空事件列表及高水位 `0` |

例如保留事件为 `3, 4, 5` 时，`after=0` 或 `1` 已过期，`after=2` 返回保留事件，`after=5` 返回空列表，`after=6` 为未来游标。保留边界和高水位均限定到传入会话，其他会话的裁剪或事件计数不影响判断。

事件仍只包含 `sequence`、`action_id`、字符串 `state_version` 和 `status`。卡片、检查点、动作结果、学生原问题与凭据不进入事件。没有修改数据库结构，没有新增会话删除、归档或历史动作淘汰机制；原有事件数量上限继续生效。

## 重启与回归

非终态会话首次重启隔离为 `RecoveryRequired` 时，版本和事件高水位各按现有规则前进；后续重启不重复增加。终态会话的高水位保持不变。检查点、幂等动作、单写者锁、额度和取消门控继续由已有事务保证。

验证命令在 `sigflowAgent` 目录执行：

```powershell
.\.venv\Scripts\python.exe -m unittest discover -s tests -p test_host_store.py
.\.venv\Scripts\python.exe -m unittest discover -s tests -p test_host_runtime.py
```

RED：扩充后的存储测试共 25 项，旧实现产生 3 个断言失败和 4 个缺字段错误，分别暴露游标拒绝缺失与 `event_high_watermark` 缺失。GREEN：存储测试 25 项通过；随后当前工作区 Runtime 原有定向回归 20 项通过。

新增断言覆盖初始整数高水位、并发状态提交下的版本/高水位一致性、裁剪边界、未来游标、会话隔离、重启隔离与终态保持，以及事件仅发布提交元数据。裁剪测试在临时数据库中缩小原有保留上限触发真实裁剪分支；未消耗真实模型额度。

## HTTP 与运行时集成结果

Runtime 的 GET state 在同一事务取得状态和高水位，向客户端编码为十进制字符串 event_high_watermark。该值是快照元数据，不是历史动作结果的一部分；重放动作后若需要重同步事件，应重新读取 state，不把旧动作结果和当前游标拼接。HTTP 对 cursor_expired/cursor_stale 均返回 410，客户端按以下顺序恢复：停止使用旧游标，读取 state 及 event_high_watermark，再以该值作为 after 续订。响应获得之后新提交的事件会以更大序号返回，不因分开读取状态与游标而漏掉。

组合 HTTP fixture 已验证：缩小事件保留窗口触发真实裁剪，旧游标得到 410，读取同一状态快照中的字符串高水位后正常续订。事件仅包含已提交元数据，不包含卡片、原问题或凭据。高水位只推进于提交成功，磁盘失败与动作预留不发布事件。

[D6](u1-d6-catalog.md) 允许同一会话重新读取可信目录；[D7](u1-d7-observation.md) 允许已提交的长 Job 显式只读观察。组合场景覆盖 Running→Succeeded但缺锚点→可信目录补入→有限反馈，Job POST 始终只有一次；撤销 grant 后仍可读取已有任务，不开放后继提交。网络瞬断、基线重新取证失败、取消、审批包隔离及旧账本迁移均有回归。

## 最终验证与阶段再规划

实测环境为本地 Windows、Python 3.13.5，标准库运行，不要求 Conda 或 UCAgent。

| 验证 | 结果 |
| --- | --- |
| 核心与宿主全量 | 368 项，362 通过、6 项历史可选实验跳过 |
| 宿主与新续查相关 | 149 项通过：store25、runtime20、HTTP16、verification49、process4、catalog11、observation19、coupled5 |
| 旧 agent 独立导入回归 | 124 项全部通过，保留其服务实现 |
| 离线开发评测 | 40 项全部通过，结果及当前源码摘要见 [evaluation-d8.json](evaluation-d8.json) |
| 独立审查 | 已复现问题修复并复审，无未解决的可操作发现 |
| 文档与语法 | 13 个文件、72 个本地链接、3 个 JSON 示例、10 个 PowerShell 语法块通过；源码 compileall 与 diff whitespace 检查通过 |

本地 HTTP 和进程测试使用真实套接字、SQLite 与明确 fixture 数据，不运行真实 EDA 或 DeepSeek，不计作真实工具或模型质量验收。

后续按耦合关系推进：S1/S2 的可信目录生产方与真实报告→真实只读观察→UI 状态/事件/审批衔接→真实受控 Job 与 provenance 对账。跨进程验证恢复必须单独设计无授权继承的持久格式；本批不把 live 会话续查冒充重启执行恢复。U3 的完整学习数据保留、导出和删除策略也不由最小事件流代替。

阶段文档、任务清单及计划在每阶段后同步；本批未提交或推送，未删除工作区外文件。

真实 Gateway/EDA/DeepSeek、Linux 文件锁、跨进程验证恢复、动态锚点可信生产方、完整功能观察比较和 UI 签收仍需各自明确验收；存储 fixture 结果不替代这些外部边界。
