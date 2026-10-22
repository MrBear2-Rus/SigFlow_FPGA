# 阶段 30：固定审阅材料与内部正文读取

`ReviewedMaterialProvider(manifest_path, manifest_sha256, trusted_directory, verify_review)` 只由可信宿主组合。manifest 的 SHA-256 是配置方提供的外部固定值，不能从同一可变文件旁自动读取。格式为 `schema_version: u2.reviewed-materials.v1` 与 `references` 数组；每项只能包含 `reference_id/project_id/revision/issue_id/policy/sha256/reviewers/not_before_epoch/expires_at_epoch`。policy 固定 `u2-reviewed-reference.v1`，同一工程/版本/issue/policy 只能匹配一项，reviewers 为 2–8 个不同标识。时间用整数 epoch 秒，左闭右开。

正文仅从配置目录的 `<reference_id>.txt` 读取，不接受 manifest 自定义路径。提供器核验目录边界、普通文件、symlink/junction、文件描述符身份、32 KiB 上限、UTF-8 和正文摘要；每次调用重读固定摘要的 manifest，并在正文 I/O 后重核元数据和有效期。`verify_review(entry)` 必须由可信系统现场检查真实教师审阅权限、范围、摘要、有效性和撤销，返回值必须为 `True`。reviewer 字符串仅为标识，不能自证教师身份。回调收到深复制，读前后都会调用；回调阻塞跨过到期时间也会拒绝。

`Runtime.reference_body(sid, learner, expected_version)` 是内部返回端口，复用 ReferenceActions、DurableReferenceGate 和 ReferenceJournal。先检查配置、learner、live 会话、L3 WaitingStudent、当前版本、无 pending、未取消、confirmed 元数据及 released journal，匹配 id/正文摘要/审阅集合摘要。两次元数据快照同时持 Runtime guard 与既有 store RLock，防止不持 Runtime guard 的 worker 在 row/pending/journal 读取之间提交，产生混合版本。所有锁都在 provider I/O 前释放，读取后重核会话对象、run、版本、pending、取消、记录和元数据。这里只返回 `{reference_id, sha256, policy, reviewers, content}`；不写状态/动作/事件/checkpoint/outbox，不再次核销，不创建 Job、模型调用或新等级。读取后公开 `body_available` 仍为 False；该字段描述默认公开宿主路径。

重启的会话拒绝读取；恢复证据后仍须显式 `reference_reconcile`。既有 released receipt 在重核时不会再次 consume。默认 HTTP 的 L4 仍关闭，没有新增 HTTP 路由、共享 schema 或平行状态机。

实际验证：新 `test_materials.py` 8 项中 7 通过，Windows 未授予创建 symlink 的权限（1314）使实际 symlink 用例跳过；另有无需提权的显式链接拒绝分支测试。`test_reference_body.py` 10 项通过，覆盖固定 manifest 提供器与真实 Runtime 组合、未确认/错范围/错版本拒绝、review 撤销、读取期间取消/版本/pending/journal/close 变化、慢 provider 不占全局锁、恢复与显式重核，以及正文不进入 DB/事件/动作和不改变预算。首轮分别先见提供器缺失 5 项失败与内部端口缺失 7 项失败，再实现。审查发现最终 review 回调可能跨期，以及元数据多次读取间 worker 提交会形成混合版本；两者均新增确定性失败测试后修复，后者使用真实 store 和并发提交线程。

定向回归由 `.superpowers/sdd/u2-stage28-31/verify_memory_materials.py` 运行新测试及原有 `test_teaching_memory.py`、`test_host_reference.py`，将旧测试默认临时目录限制到工作区；最终 50 项中 49 通过、1 项跳过、无失败（18.845 秒），完整结果见同目录日志。外部仍缺实际固定材料、真实教师审阅身份/撤销后端以及 U1/S3 注册。所有材料和身份均为明确合成契约测试，不能据此声称教师验收或生产授权已完成。可信材料目录仍应由操作系统权限防止恶意并发写入；读取围栏不代替文件系统访问控制。
