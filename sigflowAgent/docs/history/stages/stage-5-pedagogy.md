# 阶段 5：教学策略与八类模式

日期：2026-10-10。策略 `pedagogy.v1`，注册表 `patterns.v1`。

## 实现范围

`patterns.assess(evidence, facts=None)` 返回 `supported`、`excluded`、
`insufficient` 或 `unsupported`，并附稳定原因与模式。冻结的 `Pattern` 包含
concept、requirements、exclusions、detector、L1–L3 prompts、retest 和版本。
注册表只读；没有从日志、源码、课程或模型关键词构造硬件事实。

`PatternFacts(concept, anchor, detector, observations)` 是可信宿主端口输入，
不是模型输出 DTO。observations 必须是字符串 tuple，且仅允许该模式声明的事实。
anchor 必须与 Evidence 的 project、revision、job、snapshot、artifact id/hash、
evidence id 完整绑定，过期或不匹配时拒绝。当前仅接受 `synthetic.v1` 搭配
`origin=fixture`，该通道不能给真实工具认证新增 detector。
调用方仍需用当前请求验证 Evidence 的目标 revision；事实自绑定不能取代请求校验。

RuleProvider 保留既有锁存器 L1–L3 文本，可选 `facts_source` 是可信宿主回调。
默认没有回调。主循环在默认 assess 不支持、排除或证据不足时绕过模型，给出保守提问；
支持模式的模型输出仍经过 checked_card。L4 保持不可用。

## 支持矩阵

| 模式 | 必需事实 | 合法排除例 | 当前真实 detector |
|---|---|---|---|
| syntax | parser_diagnostic、source_location | accepted_language_construct | 未接入 |
| latch | storage_observed、combinational_goal | intentional_storage | 报告适配仅映射 EDU_LATCH_INFERRED |
| width_signedness | operand_types、expression_width、expected_range | intentional_conversion | 未接入 |
| drivers | driver_inventory、net_kind | legal_resolved_net | 未接入 |
| combinational_feedback | dependency_cycle、combinational_path | sequential_boundary | 未接入 |
| ports | interface_declaration、connection_map | intentional_open_port | 未接入 |
| reset | reset_contract 与精确波形条件 | contract_allows_behavior | 未接入 |
| simulation_mismatch | expected_result、observed_result 与精确波形条件 | comparison_matches | 未接入 |

波形条件为 exact、complete、all_pages、initial_values、timescale，且必须绑定完整
artifact/revision。缺少任意条件不可给确定性波形诊断。条件是开发 fixture 的显式事实，
不是当前系统已经能查询精确波形的证明。真实 S2 查询、分页闭合和事件比较仍未实现。
默认 latch 兼容入口基于已有可信 Evidence source 契约；普通文本不能创建该 concept。
ReportEvidenceSource 对其它诊断代码仍映射 unknown。

## 验证与来源

执行：`.\.venv\Scripts\python.exe -m unittest discover -s tests -p test_patterns.py -v`。
实际结果：31 tests，OK。首轮缺失注册表时 30 项失败；新增过期 anchor 回归时
观察到 1 项预期失败，修复后全部通过。

其中 24 个独立命名场景来自 tests/test_patterns.py 的手写合成数据：八类各一个支持例、
合法反例、缺证据例。其余覆盖七项绑定字段变更、合成 detector 不得认证 tool origin、
畸形事实、波形逐条件缺失、默认保守行为、L1–L3 检查及越级拒绝、过期锚点。
这些案例验证教学路由与证据边界，不能证明硬件检测正确性。

并行实现期间全量运行曾为 109 tests / 3 failures / 6 skipped；三项是主循环模型门禁
实现前新增的预期失败。随后同一全量命令复测：发现 109 项，103 项通过、6 项跳过。
模型门禁回归覆盖未知模式、未接入 reset 和合法锁存器反例；三种情况均不调用模型。
独立审查未发现阶段 5 的额外阻塞缺陷。可选集成结果以阶段 6 报告为准。

## 再规划

UA-06 的策略文档与本地提示边界、UA-07 的八类注册表和合成正反例已实现。
真实八类检测、精确波形诊断、教师语义评审及可信 receipt 的 L4 仍未验收。
S2 需交付结构化 detector 身份、对应证据与精确事件查询；U3 需独立参考内容和冻结集。
新增真实 detector 必须显式扩展适配与绑定验证，不能仅打开布尔开关。
