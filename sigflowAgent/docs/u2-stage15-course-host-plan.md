# U2 阶段 15：课程上下文进入常驻宿主

日期：2026-10-16。目标：将 U3 已交付的只读课程数据库接入 U2 常驻教学会话，使真实报告、工程证据和课程背景在同一工作流中可观察，同时保持重启、预算与受控执行边界。

## 设计与边界

采用现有 CourseProvider 端口与 SQLiteCourseProvider，不新增检索算法，不改 U3 数据库。只在 E3 准备课程，E4 将保留片段和引用白名单交给模型；工程证据与课程叠加上下文分开保存。工程证据摘要、原始报告 occurrence、计划与执行恢复不依赖课程检索结果，因此课程无匹配或变化不会制造新的工程事实。

相比将课程拼进报告 source（会污染固定报告摘要），或另开独立课程服务（无法直接服务当前教学会话），本阶段在 TeachingLoop 内保持单独的课程上下文。state 提供当前课程状态、保留的引用元数据及省略情况；不返回课程原文或检索调试轨迹，不增加模型/HTTP 注入 CourseProvider 的入口。

课程启用的只读检查点保存同级规则卡的恢复材料，不保存原模型的课程解释或历史课程引用；重启后先认证并核对同一工程观察，再重新检索当前课程背景，不调用模型、不升等级。验证恢复同样保持原工程摘要与 GET-only 观察，重新取得背景但不继承授权。未开启课程的历史 v1 检查点语义和格式保持兼容。

宿主默认使用随仓库的只读数据库，可由可信启动参数 `--course-db` 指定，也可 `--no-course` 禁用。HTTP 请求不接受数据库路径。capabilities 区分配置启用与本轮检索结果；不能以配置启用声称检索成功、教师审阅完成或问题可回答。

## 全局约束

- 新服务、测试和文档位于 sigflowAgent；保留 agent，工作基线 8b067b2。
- Python >=3.11 标准库；课程 SQLite 需 FTS5 trigram。无 Conda/UCAgent 依赖，不下载课程原文，不重建数据库。
- 不提交、不推送，不修改两份已有 stash，不删除无关文件。
- 课程不可提供执行授权或改变工程证据归属；模型调用预算、L1–L3/L4 默认拒绝保持不变。
- 原文、问题正文和旧课程引用不进入检查点；恢复没有模型调用或 Job POST。
- 默认现有闭环 CLI 行为保持兼容，增加显式课程验收选项。

## Task 1：工程观察与课程叠加分离，恢复重新取背景

文件：loop.py、courses.py、runtime/checkpoint.py、runtime/verification_checkpoint.py、patterns/__init__.py；新 tests/test_course_recovery.py。

- [x] 测试先行：E3/E4 Provider 得到课程片段，而 loop._evidence 仍是原始工程观察；课程省略不禁用完整 job_observation，工程省略仍拒绝模型。
- [x] TeachingLoop 新增 `_course_evidence` 与保留引用；每轮 E3 重新准备，不在工程证据上累加课程，E6 始终使用工程观察。
- [x] 课程启用时，readonly checkpoint 使用原始工程摘要并保存同级规则卡；恢复卡不得带回旧课程解释、原文或引用，原模型预算不重置。关闭课程时原有保存规则不变。
- [x] restore_readonly_session 接受可信 course_provider 端口；显式 reconcile 在工程摘要匹配后重新准备背景。工程不匹配时课程不查询、卡片不发布。
- [x] 验证恢复沿用 fresh loop 的 course_provider，恢复同样重新准备背景，不调用模型或执行；历史检查点没有课程字段，不扩展格式。
- [x] 覆盖课程数据库变更/缺失、查询失败、无匹配、重复 hint、重启及工程摘要篡改，运行既有课程/检查点/验证恢复回归。

## Task 2：生产组装、状态与真实工具联合验收

文件：host/factory.py、host/__main__.py、host/runtime.py、host/http.py、host/closed_loop.py；新 host/course_state.py、host/course_acceptance.py；新 tests/test_host_courses.py、tests/test_host_process.py 扩充。

- [x] make_factory 增加可信 course_provider 可选参数，传入 TeachingLoop；Runtime 恢复传递相同端口。
- [x] 课程配置仅来自可信启动参数；默认 SQLiteCourseProvider，--no-course 明确禁用；错误数据库只导致本轮 unavailable，不伪造成功。
- [x] capabilities.course 表示 enabled/disabled 配置状态；state.course 提供检索/装配状态、当前可用引用元数据、缺失来源及省略计数，不含原文、路径或 debug。
- [x] RecoveryRequired/取消时隐藏旧引用，防止 GET 或 create/action 重放泄露尚未重验的课程；learner/project 隔离保持原样。
- [x] runner 增加 --courses 选项与可选 --course-db；启用时发送明确的 Yosys 课程检索问题，要求真实 Job 范围不变、课程非空且元数据来自数据库，并输出脱敏联合验收事实。规则初始卡可不引用课程，state 的背景材料与卡片实际引用分开。
- [x] 定向测试红/绿后运行全量、开发评测、真实 Core/Yosys+课程场景，独立审查并修复实际发现。
- [x] 产出阶段 15 开发手册、机器报告与源码摘要，更新 task.md/README；单独列出 GUI、真实仿真/波形、L4 和教师质量等剩余范围。

## 后续再规划

课程背景进入真实工作流后，下一阶段按 UA-07 推进可信波形读取与预测动作；真实 detector 和完整性缺口仍需逐项补证。UA-08 的教师评分不能由本次数据库集成测试替代。

## 执行与验收记录

Task 1 新测试先失败（7 项中 3 个断言失败、4 个缺少接口错误），实现后通过；只读恢复、验证恢复和课程用例完成回归。Task 2 先复现工厂缺少课程端口；真实数据库进一步暴露内部 job_observation 不属于硬件概念的问题，仅在 U2 转换层过滤该中性标记，保留问题文本检索。未改变 U3 检索算法或数据库。

独立审查复现成功恢复后历史动作带回旧引用的问题，新增失败回归后以持久 recovery_version 边界修复，复核 PASS；覆盖后续 hint/finish、持久库重开及边界后动作的历史结果语义。最终 unittest 报告运行 503 项、35 条跳过记录，69.867 秒，无失败：逐项成功 470，单项跳过 33，另有 2 个类级跳过覆盖 18 个未运行方法。开发评测 40 项通过，源码摘要一致。

真实 Core/Yosys 加课程场景通过：2 Job、4 条数据库核验背景、partial/not_assessed、规则卡实际引用 0、模型调用 0。关闭课程的跨版本场景另通过 4 Job 回归。阶段说明见[开发手册](stage-15-course-host.md)，证据及摘要见[验收汇总](verification-stage15.json)。本阶段各项完成；U2 整体仍按 UA-06/07/08 保留未验收范围，不提交、不推送。
