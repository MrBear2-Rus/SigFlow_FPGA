# U2 阶段 4–6 实施计划

> 执行方式：subagent-driven-development；逐任务实现、审查、验证和再规划。

**Goal:** 完成独立可测的计划与结果检查、八类教学模式、离线对抗评测。

**Architecture:** 保持 TeachingLoop 作为主循环，纯逻辑检查器通过领域类型接入；
模式注册表仅提供有证据边界的教学策略，评测 runner 调用真实组件。

**Tech Stack:** Python >=3.11，标准库 unittest；UCAgent 集成为可选测试组。

## Global Constraints

- 已批准设计：[设计](u2-next-stages-design.md)。
- 仅在当前 sigflowAgent 工作；保留 agent、共享协议与既有无关改动。
- 不 push、不删除工作区外文件；本批改动留在工作区供审阅。
- 原 task.md 的真实工具、UI、教师与完整运行时验收不以离线测试替代。

## Task 4：计划和复测检查

Files：新增 checkers/plan.py、checkers/outcome.py、对应 tests/test_checkers.py；
修改 domain.py、loop.py 的最小接线。产物 docs/stage-4-verification.md。

Consumes：RunRequest、Evidence、Plan、VerificationResult、VerificationPort。
Produces：详细计划模型、校验及稳定摘要、旧新证据比较；保留旧 Plan 用法。

- [x] 先写非法参数/依赖/超预算/版本混用/旧 Job 测试，运行确认失败。
- [x] 实现受限参数和最多 3 Job 的详细计划，绑定 snapshot 和预期观察。
- [x] 实现 OutcomeChecker，并接入循环，不符反馈可重新取证且不重用旧授权。
- [x] 核对详细计划校验在执行前生效，不可变模型不能被外部字典修改绕过。
- [x] 跑新增测试与全部回归，记录结果；审查后写阶段报告。
- [x] 对照 UA-05 再规划，不勾选真实执行闭环。

关键验收行为：`with self.assertRaises(ValueError): ...` 拒绝未知参数和循环依赖；
同内容计划摘要一致，任一授权相关字段变化摘要不同；
旧 Job 和跨项目结果不可产生“符合预期”结论。

## Task 5：模式与策略

Files：patterns/ 注册表、tests/test_patterns.py、pedagogy.py 最小接线；
docs/stage-5-pedagogy.md 与 policy 说明。

Consumes：CardContext、Evidence 和 checked_card。
Produces：版本化八类模式、支持/排除/证据不足判定、L1–L3 模板。

- [x] 写八类各三个有独立预期的合成场景，运行确认新增能力缺失。
- [x] 实现注册表和规则输出；未知 detector/缺证据走保守提问。
- [x] 保留锁存器现有行为，L4 仍拒绝；波形无完整精确证据不能确诊。
- [x] 运行新场景与全量回归；审查支持边界，记录真实 detector 缺口。
- [x] 对照 UA-06/07 再规划并输出阶段文档。

关键验收行为：合法反例不被描述成已确定的错误；证据不足不宣称根因；
每次 hint_next 最多一级，模式选择不能绕过 checked_card。

## Task 6：离线评测

Files：evaluation/ runner、tests/test_evaluation.py、开发评测 fixtures；
docs/stage-6-evaluation.md、教师评审表及结果文件。

Consumes：前两阶段真实组件、TeachingLoop、checked_card。
Produces：可机器读取的评测报告与有意义的进程退出码。

- [x] 先测试错误预期导致 failed>0/非零退出码，禁止 runner 恒成功。
- [x] 增加注入、伪确认、证据/等级/预算/动作重放场景；脱敏输出。
- [x] 记录版本和证据类型；默认离线，真实模型与教师评分标未执行。
- [x] 运行开发评测、全量回归及依赖检查，保存本轮证据。
- [x] 最终审查代码/文档一致性并更新本地 task.md 与剩余依赖。

运行入口（sigflowAgent 目录）：

```powershell
.\.venv\Scripts\python.exe -m unittest discover -s tests -v
$env:SIGFLOW_UCAGENT_SOURCE = (Resolve-Path '..\..\UCAgent\UCAgent').Path
.\.venv\Scripts\python.exe -m unittest discover -s tests -v
```

第一条在未设置上游路径时将跳过可选集成；第二条必须单独记录集成实测结果。
