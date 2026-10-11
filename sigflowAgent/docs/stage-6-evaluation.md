# 阶段 6：离线评测与阶段交接

日期：2026-10-10。基线提交：`29bef05`；本批代码保留在工作区，未提交或推送。

## 交付范围

`sigflow_edu_agent.evaluation` 提供开发评测 runner，调用实际 TeachingLoop、
checked_card、Plan/DetailedPlan、OutcomeChecker 与 patterns.assess。
数据集 [development.json](../tests/evaluation/development.json) 为合成开发集，
不属于 U3 冻结验收集，不包含真实学生或工程数据。

报告 [evaluation-development.json](evaluation-development.json) 保存 40 个用例的实际结果，
包括数据集摘要、源码内容摘要、工作流/策略标识、逐项预期/实际/原因与分组计数。
源码摘要遍历本包 Python 源文件，按相对路径排序，统一 CRLF/LF 后计算；
工作区尚未提交也能识别本轮源码。upstream/model/RAG/prompt 未参与本批评测，
明确写 not_used，temperature 为 null。

## 运行方式

工作目录为 `sigflowAgent`；无需模型 Key、Gateway 或 UCAgent：

```powershell
.\.venv\Scripts\python.exe -m sigflow_edu_agent.evaluation --suite tests/evaluation/development.json
```

标准输出是 JSON；退出码 0 表示用例通过，1 表示至少一个用例不符合预期，
2 表示输入无效。输入限制为 64 KiB、最多 256 个场景，probe 必须来自固定清单，
不能提交可执行代码、任意模块名或动态表达式。未知字段、重复 ID、非布尔预期拒绝。
runner 不创建输出文件；保存结果应由开发者显式重定向，注意选择编码。

报告只包含受限场景 ID、预定义 probe、布尔/空实际值和固定原因。
异常正文、异常返回对象及输入原文均不进入报告。

## 实测结果

| 证据类型 | 场景数 | 通过 | 失败 | 说明 |
|---|---:|---:|---:|---|
| 确定性规则与检查器 | 37 | 37 | 0 | 教学边界、参数、引用、版本、动作、模式 |
| Mock Provider | 3 | 3 | 0 | 调用预算、修正次数、模型错误降级 |
| 真实 EDA 工具 | 0 | 0 | 0 | 未执行，不能算通过 |
| 真实 DeepSeek | 0 | 0 | 0 | 未执行，不能算通过 |
| 教师评审 | 0 | 0 | 0 | 未执行，评审表已提供 |

覆盖越级、伪造/额外引用、代码输出、非法工具字段、空/超限内容、旧版本、
跨项目、假确认、重复动作、取消、预算、注入文本、计划依赖/摘要、重复 Job、
模式事实跨锚点和波形条件不完整。
注入场景验证规则路径不执行或回显数据中的命令；不声称真实模型具有同等免疫性。

独立评测测试 7 项通过，包括故意错误预期引发失败及非零 CLI 退出、坏输入、
非布尔敏感返回值脱敏、异常脱敏、输入体积与计数核对。
独立审查发现并修复返回值未归一化、规则/Mock 未分组两个问题；复审通过。

最终回归命令：

```powershell
Remove-Item Env:SIGFLOW_UCAGENT_SOURCE -ErrorAction SilentlyContinue
.\.venv\Scripts\python.exe -S -m unittest discover -s tests -q
$env:SIGFLOW_UCAGENT_SOURCE = (Resolve-Path '..\..\UCAgent\UCAgent').Path
.\.venv\Scripts\python.exe -m unittest discover -s tests -q
.\.venv\Scripts\python.exe -m pip check
```

- 核心隔离：发现 114 项，108 通过，6 项可选集成跳过。
- 含真实 UCAgent 类集成：114 项全部通过，无跳过。
- pip check：No broken requirements found。
- 真实 UCAgent 类测试不代表完整 StageManager/profile、真实模型或 EDA 已验收。
- 最终整体代码审查无新增阻塞问题；发现的公开端口注解解析兼容性问题已补回归修复。
- 17 份 Markdown 的相对链接、代码围栏及尾部空白检查通过。

## 再规划与交接

| 原任务 | 本批新增本地交付 | 仍需交付 |
|---|---|---|
| UA-05 | 详细计划校验、前后证据核对、多步有限结论 | U1 执行适配与持久化；S1/S2 grant、真实新旧 Job |
| UA-06 | policy 文档、模式门禁与对抗场景 | S1/S3 receipt 双确认、原子核销及恢复 |
| UA-07 | 八类注册表、24 个合成三类场景 | S2 真实 detector、精确波形；U3 审阅材料 |
| UA-08 | 可复现 runner、40 个开发场景、教师表 | U3 冻结集、真实模型每场景至少 3 次、双人教师评审 |

阶段 4–6 的本地交付完成，不将原团队任务整体勾选完成。
下一轮优先对齐 U1/S1 的详细计划映射和 grant 摘要规则，再用 S2 的真实
锁存器前后报告跑只读核验，最后接入受控 Job；L4 必须等完整 receipt/恢复边界。
八类真实检测接入前保持 unsupported，不能仅通过开关启用合成事实通道。

人工评审使用 [teacher-review.md](teacher-review.md)，未填写评分即未完成评审。
