# 阶段 15：常驻宿主的课程背景与恢复

日期：2026-10-16。范围对应 UA-05 上下文组装，以及 U2 对 U3 课程端口的生产组装。设计与步骤见 [阶段计划](u2-stage15-course-host-plan.md)。

## 行为

常驻宿主默认使用 U3 随仓库交付的 `data/course.sqlite`。E3 调用现有只读课程端口，E4 将保留的片段与引用白名单交给规则/模型流程。课程不可替代工程报告、改变工具事实或签发执行授权。内部 `job_observation` 是任务观察类型，检索时不作为硬件概念提交；课程仍按问题文本查找，不生成虚假硬件诊断。

`TeachingLoop._evidence` 保留固定工程观察，`_course_evidence` 仅保存当轮教学上下文。工程检查点摘要、occurrence、E6 取证和结果比较继续绑定原工程事实；课程省略、数据库变化或无匹配不会改变报告身份。完整任务观察仅忽略合法的课程预算省略，缺失工程观察仍不能打开模型门禁。

缺库、库不可用或检索异常时课程状态为 unavailable；无匹配为 no_match。两者继续提供基于工程证据的教学流程，不伪造课程引用。课程查询不能开启 L4，不能触发 Job，也不增加模型预算。

## 可信配置

在既有宿主启动参数后可增加：

```powershell
# 默认启用随仓库的课程库；也可通过可信启动环境 SIGFLOW_COURSE_DB 指定。
& .\sigflowAgent\.venv\Scripts\python.exe -P -m sigflow_edu_agent.host --bootstrap-stdin --gateway-evidence

# 显式数据库位置，仅由启动宿主提供。
& .\sigflowAgent\.venv\Scripts\python.exe -P -m sigflow_edu_agent.host --bootstrap-stdin --gateway-evidence --course-db .\sigflowAgent\data\course.sqlite

# 明确禁用课程检索。
& .\sigflowAgent\.venv\Scripts\python.exe -P -m sigflow_edu_agent.host --bootstrap-stdin --gateway-evidence --no-course
```

以上命令仍需通过 stdin 交付 D5 规定的 bootstrap，不在命令中传令牌。`--course-db` 与 `--no-course` 互斥；HTTP create/action 不接受数据库路径、Provider 或课程片段。构造 make_factory 的可信调用者可传 `course_provider=`，不传则保持原有禁用行为。常驻 CLI 的默认启用与 Python 组装函数的向后兼容默认值不同。

课程运行依赖仍是 Python >=3.11 标准库与支持 FTS5 trigram 的 SQLite；无需 Conda/UCAgent，不下载课程原文，不重建数据库。原文重建和课程审阅属于独立工作。

## 状态契约

接口继续使用私有 `u2.host.v1` 扩展；不修改共享 edu.api.v1 的执行授权语义。

| 位置 | 字段/含义 |
| --- | --- |
| GET /api/v1/capabilities | course.available 表示已配置端口；reason 为 configured/disabled，不表示数据库可读、检索成功或教师审核完成 |
| 会话 state.course.status | disabled、ok、partial、omitted、no_match、unavailable，恢复和取消投影另用 revalidation_required/cancelled |
| state.course.references | 当前保留课程背景的 CourseReference 元数据；不含课程正文 |
| retrieval_status / packing_status | 区分检索结果和装配后的范围；恢复隔离期为 not_checked |
| answerability | 始终 not_assessed；结构与引用检查不能证明材料足以回答问题 |
| omitted / omission_counts | 本轮被省略课程及原因；工程证据不因课程优先级而被挤掉 |
| missing_required_sources / missing_required_evidence | 课程侧已有的缺口诊断，不提供内部 debug 或排序轨迹 |
| cards[].course_refs | 该卡实际引用的白名单子集，与 state.course.references 的可用背景材料分开 |

规则卡可没有课程引用，即使 state 中存在课程背景；不能将这些背景包装成模型实际引用。引用的 HTTPS 或 sigflow-note 元数据定位保持可用，普通 hint/question 继续经过路径和令牌脱敏。learner/project 的状态访问限制保持原样。

## 重启与重放

只读 v1 检查点继续保存工程摘要。启用课程时，检查点保存同等级规则卡作为恢复材料，不保存旧模型课程解释、原文或引用；不修改当前已发布卡片，也不重置已用模型预算。

重启先进入 RecoveryRequired，GET 和旧动作重放隐藏卡片及课程引用。显式 recover 在同一工程摘要校验通过后重新准备当前课程背景，不调用模型，不提高提示等级。课程数据库升级或丢失不阻断已验证的工程恢复：新材料重新校验，失败则标 unavailable。工程观察不匹配时继续隔离，且不查询课程。

成功恢复的 state 另保存十进制字符串 recovery_version 作为发布边界。早于该边界的历史动作继续按原指纹幂等确认，但其 result 返回当前已重验投影，不能重新发布旧课程引用或旧模型解释；边界随状态持久化并经过后续动作保留。边界之后的动作仍按原有历史结果规则重放。修复前已通过最小用例复现“新课程恢复后旧 hint 动作带回旧引用”，修复后覆盖重复重放、旧模型解释隔离和第二次重启。

验证会话保持相同原则：恢复只读已知 Job、重新取得课程背景；不能授权或提交后继步骤。后继仍需新的显式审批和有效 grant。取消状态不展示旧课程引用。

## 真实联合验收

仓库根目录执行；CMake 宿主复用 [D11](u1-d11-closed-loop.md) 已构建目标：

```powershell
$env:PYTHONPATH=(Resolve-Path sigflowAgent).Path
& .\sigflowAgent\.venv\Scripts\python.exe -P -m sigflow_edu_agent.host.closed_loop --gateway build-main-msvcrt/sigflowAgent/integration/edu_u2_gateway.exe --mode rule --courses --timeout 120 --output sigflowAgent/docs/closed-loop-stage15-courses.json
```

可用 `--course-db` 选择验收数据库，必须同时指定 `--courses`。未指定 `--courses` 的验收运行器显式关闭检索，保留 D11/D12 默认场景；常驻宿主 CLI 本身默认启用课程。

实测完成两个真实 Core/Yosys Job、教学审批和新工具证据反馈；课程背景保留 4 条、status=partial、answerability=not_assessed。运行器按当前数据库核对引用编号、父材料、索引版本、标题、行号、hash 和 URL，metadata_verified=true。初始规则卡的实际课程引用数为 0，模型调用数为 0；报告不包含课程正文、问题、令牌或原始模型卡片。机器证据见 [联合报告](closed-loop-stage15-courses.json)。

跨版本四 Job 场景也已保留单独回归，见 [阶段 15 跨版本报告](closed-loop-stage15-student-edit.json)。它关闭课程，不将单独通过的两个场景冒充 GUI 完整体验。

## 验证与剩余范围

新增回归覆盖工程/课程摘要分离、同级规则恢复、材料更新/缺失、禁止盲目重放旧引用、验证恢复无 POST/模型调用、配置、learner 隔离、HTTP 注入拒绝、HTTPS 定位与真实数据库元数据篡改拒绝。已有课程端口与教材重建测试继续使用原有断言。

最终 unittest 报告运行 503 项、35 条跳过记录，无失败，耗时 69.867 秒。逐项成功 470、单项跳过 33（6 项历史 UCAgent 实验、27 项缺课程原始快照）；另外 2 个 setUpClass 跳过覆盖 18 个未运行方法。因此不能用 503−35 计算成功数。共收集 521 个测试方法，未运行方法不计作通过。正常 Windows 当前用户上下文下的 DPAPI 测试仅使用虚构 Key。离线开发评测 40 项通过（37 规则、3 Mock），源码摘要与最终源码一致；评测本身没有真实模型或工具调用。独立审查发现的恢复后旧动作重放漏洞已修复并复核 PASS。完整摘要、命令和范围见[阶段验收记录](verification-stage15.json)，逐项开发评测见[评测报告](evaluation-stage15.json)。

本阶段不宣称课程材料完整或教师已评审，不验收真实 DeepSeek 课程生成质量、GUI、真实 Verilator/波形、硬件功能正确性、L4 或跨平台发布。课程数据库与 U3 检索算法均未修改。本批不提交、不推送。
