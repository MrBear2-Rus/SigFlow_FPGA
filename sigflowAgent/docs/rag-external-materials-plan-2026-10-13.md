# 外部课程资料引入计划

日期：2026-10-13
状态：已实施并完成本地开发验证；教师审阅、许可复核和真实模型/EDA验证仍未完成。执行结果与差异见[实施记录](external-materials-catalog-2026-10-13.md)。

## 一、当前基线

正式实现已于 2026-10-12 迁入本项目，入口见 [rag-integration.md](rag-integration.md)。

- 运行包：sigflow_edu_agent/rag/
- 构建资料：sigflow_edu_agent/rag/resources/courses/
- 当前库：data/course.sqlite，user_version=3。
- 本次只读统计：257 条 YSYX 原文材料、9 条外部 authored_note，共 266 条；HDLBits 练习继续独立。
- 保持单文件数据库，临时文件构建后替换当前库；不恢复旧 U3 活动 manifest、多版本数据库、历史兼容与回滚入口。
- 内容 hash 标识继续用于引用更新检测；新增原文使用新 passage_id，已有引用不得悄悄改指另一种材料。
- 旧 U3 中的官方资料规模/许可核查是审查记录，不是当前运行入口。

## 二、中文版检索结果及采用决定

本次检索覆盖官方网站、公开网页与 GitHub 仓库检索，不声称已穷尽所有私人或未收录译文。

| 资料 | 发现 | 采用决定 |
| --- | --- | --- |
| lowRISC Verilog 指南 | 暂未确认与 9c15ff5 对应的完整中文译本；官方目录列出英文指南 | 以固定英文原文为证据，自建中文标题、术语和学生问法映射；核心片段另做对照译文 |
| Yosys | 找到 alain6666/Yosys-Chinese-Manual；树中只有 readme.md 与序言正文，内容存在重复；作者提示以官方原文为准；最后推送为 2023-12-12，未注明对应 0.47 | 作为中文阅读链接候选；不能当成完整手册。没有独立 LICENSE，文中对 Yosys ISC 的介绍不等于对译文本身的许可声明，暂不复制译文入库 |
| Verilator | 暂未确认与 v5.052 对应的完整中文译本；找到 CHIPLAB、YSYX 的中文使用资料 | 项目使用说明不冒充官方手册翻译；当前 YSYX 内容继续使用，其他项目资料暂不纳入通用工具正文 |

依据：
- [lowRISC 官方目录](https://github.com/lowRISC/style-guides)
- [Yosys 社区译文](https://github.com/alain6666/Yosys-Chinese-Manual)
- [译文仓库文件树](https://github.com/alain6666/Yosys-Chinese-Manual/tree/master)
- [Verilator 官方指南](https://verilator.org/guide/latest/)
- [CHIPLAB 中文文档](https://chiplab.readthedocs.io/zh-cn/latest/index.html)

其他中文候选只登记为参考：
- [thu-cs-lab/verilog-coding-standard](https://github.com/thu-cs-lab/verilog-coding-standard)：独立的中文编码建议，不是 lowRISC 译本；本次未发现独立 LICENSE，部分风格选择与 lowRISC 不同，不直接混用。
- [ZION VerilogCodingStyle 前言](https://github.com/zion-group/VerilogCodingStyle/blob/master/source/Preface.rst)：中文团队规范，不是 lowRISC 译本；前言声明 CC-BY-ND，未写清具体许可版本。只列阅读链接，暂不切分改编入库。

没有发现可直接使用的完整译本，不是停止英文正文入库的理由。社区译文的使用条件和官方原文的使用条件分别登记。

## 三、首批范围

先处理此前确认的六份内容，随后按教学相关性扩展：

| 来源 | 首批 | 第二批或按需 | 暂不进入学生默认检索 |
| --- | --- | --- | --- |
| lowRISC | VerilogCodingStyle.md 全文规范化；按规则、例子和例外登记。组合/时序、赋值、signed、位宽、端口、复位、case、FSM 为高优先级 | 基础验证指南中实际需要的测试激励说明 | 纯排版命名细节低优先级；复杂验证框架、团队协作规范 |
| Yosys 0.47 | primer；check 和 read_verilog 命令帮助 | hierarchy、相关 proc 命令、综合入门、资源统计、FSM/存储器相关说明 | 插件开发、综合器内部实现、完整 RTLIL、无关器件后端和 ASIC 流程 |
| Verilator v5.052 | warnings.rst、languages.rst；告警正文全量登记，优先审阅一期相关告警；解析 50 个告警示例/诊断依赖 | SigFlow 实际使用参数、波形与初始化配置、基础仿真示例 | 高级 C++/SystemC 集成、性能剖析、贡献流程 |

版权、许可、版本记录始终保留。暂不进默认检索不等于删除来源。工具文档不能充当当前工程事实，项目编码风格不能自动变成课程判错标准。

版本核对结果：知识库分别固定 lowRISC commit、Yosys 0.47 与 Verilator v5.052。SigFlow 的 Yosys 集成代码和历史真实工具验收记录标为 MinGW Yosys 0.49；本次不运行 EDA，当前可执行文件版本未现场重查。Verilator 项目记录中曾以 v5.052 完成真实 sim.build，但当前 `external/fpga-tools/runtime/` 没有 Verilator 可执行文件，本次也未运行；不得将资料版本写成当前运行版本。检索记录保留资料版本，工程报告才可证明当前运行版本。

## 四、语言处理方案

1. 英文原文保留不改，作为可引用证据；关键标识符、代码、告警码、参数不翻译。
2. 每个可检索片段补中文标题、中英文术语、concept_id 和常见学生问法；优先复用现有 concepts.yaml/别名能力，不新造第二套概念系统。
3. 首批对约 20–30 个高频核心片段制作逐段对应的中文译文。这是译文审校的工作目标，不是 passage 总数，也不是已完成数量。
4. 译文记录对应原文 ID、原文 hash、译者/生成方式、审阅状态和修改说明；未审阅译文不代替官方证据。must/should/may、条件、反例和版本必须保留。
5. 首批运行上下文以英文证据为主，中文映射负责召回；有经审阅译文时按预算选择呈现，避免中英文双份全文重复。
6. 当前公共材料类型只支持 source_excerpt/authored_note，不能把译文伪标成未修改的原文。若将译文送入 Agent，单独评审一个最小材料标记与渲染变更，明确“非官方翻译”，并保留原文引用。
7. 不要求每次提问先调用 LLM 翻译；规则映射先解决术语和常见问法。评测证明确实需要时，再利用 U2 已有模型调用提出多种检索表达，保留原问题，预算内执行。

## 五、实施步骤与交付

### 阶段 A：固定来源与许可

- 按文件清单获取官方 Markdown/RST 或命令帮助源，固定 commit/tag，保留 raw 文件、hash、取得时间和许可证据。
- 许可核对结果已写入逐文件清单：lowRISC pinned README 明确所有仓库文件采用 CC BY 4.0；Yosys 两个帮助所在 C++ 文件带 ISC 声明，primer 无文件级声明、按仓库 COPYING 记为继承判断；Verilator 两个指南文件各有 dual-license SPDX，50 个生成 include 无文件级声明、标为继承许可未确认并限制本地使用。
- 不整站爬取教材，不顺手复制网页外链论文、图片或商业工具内容。
- 形成来源清单、正文规模表、许可表、未取得/未确认项。

### 阶段 B：规范化与结构切分

- Markdown 保留标题、代码、列表和例外。RST 转换为可检索正文，同时保留原始文件路径及行范围到规范化文本的对应关系。
- 展开固定来源的 include，验证路径在允许目录内；每个被包含例子保留自己的文件与位置，不把所有内容误标为主文件行号。
- Yosys 帮助从确定的 help 输出或源文件帮助字符串生成，保留原始来源位置，不把整段 C++ 实现当成手册。
- lowRISC 一条规则连同例子/例外，Yosys 一个完整小节或命令，Verilator 一个告警码或语言主题。
- 长片段按完整子主题拆分，代码不截断。标题、解释、条件缺失时标记待补齐，不为增加条数而放行。
- 输出 passage 目录：ID、标题层级、来源位置、字符/字节数、含代码情况、中文映射、许可及审阅状态。实际数量切分后统计。

### 阶段 C：最小接入正式构建器

- 复用 source_documents、passage_spans、概念和引用表；先增加官方外部快照的构建适配，不把官方正文继续伪装成 team note。
- 主要修改点预计为 rag/source_evidence.py、rag/importer.py、资料登记及概念别名；具体新增字段以实施时验证为准，不预设大 schema。
- raw/clean 正文放在独立资料快照目录，运行只读 SQLite，模型仍不接收本机路径。
- 现有 9 条 authored_note 保留其身份和审阅状态；有直接原文支持时优先原文，避免两种材料重复占满三个名额。
- 不删除已有知识点 ID、unsupported 概念和 no_match 机制。

### 阶段 D：检索及 Agent 上下文

- 中文问题不能因 language=zh-CN 过滤而排除英文原文；正文语言与回答语言分开处理。
- 在现有概念/全文检索上补中英文映射；告警码优先精确命中，工具版本和适用范围用于过滤或排序。
- 原文、中文映射及译文作为同一证据单位处理，避免以三个 passage 重复返回。
- 保留 top_k≤3 与本轮允许引用的 ID 集合。
- 当前 `courses.pack_courses` 保留完整片段，课程正文上限为 8192 UTF-8 字节；命中后因预算省略记为 `partial/omitted` 并记录原因，不能报告成 `no_match`。DeepSeek 整体请求上限仍为 16 KiB，没有扩大。
- 工程证据优先，检索上限仍为三段；正式执行链路已验证译文单份进入中文上下文，并且引用回到原文。
- 不将教材中类似作业完整解答的内容绕过 L4 控制装入上下文；普通概念例子与参考解法逐条区分。

### 阶段 E：验收与替换当前库

开发验收已执行，教学/发布验收仍待做：
- 新增测试核对固定文件 hash、行范围、include 定位、规范化帮助文本、双语检索、版本隔离、完整上下文、预算省略、引用 allow-list、unsupported 和 no_match；49 项 RAG/Agent 相关回归通过。
- 完整测试发现 5 项 Gateway 本地 HTTP 测试因当前沙箱禁止 localhost 连接而失败，另有 6 项 UCAgent 集成测试因未配置可选依赖而跳过；详见实施记录。
- 未比较三种检索配置的整体召回/回答充分性；少量查询用例只证明链路功能，不代表总体教学效果。
- 最终库已按临时构建机制更新为 v4。测试通过不等于教师验收或真实学生效果通过。
- 资料公开分发另按各来源许可处理；含 YSYX 的混合库仍不能因为外部资料许可明确就整体公开。

## 六、协作边界

- U3：来源与许可、正文整理、映射、切分、导入、引用和检索评测。
- U2：现有问题/概念输入、真实诊断语境、教学提示及上下文预算、需要时的检索表达生成。第一批优先复用现有 prepare(question, concept_ids)，不新增必需接口。
- SigFlow 团队：提供真实工具版本、诊断和工程证据；RAG 不替代检测器。
- 团队/教师：核心译文、硬件解释和提示边界审阅。开发者审阅不能标为教师审定。

## 七、实施结果索引

- 文件、hash、许可依据和审阅状态：[外部来源清单](../sigflow_edu_agent/rag/resources/courses/external_source_files.jsonl)。
- 逐 passage 的原文范围、中英文映射和译文状态：同目录下 `external_official_{documents,passages,spans,mappings,translations}.jsonl`。
- 数量、切分规则、版本差异、上下文示例和测试结果：[实施记录](external-materials-catalog-2026-10-13.md)。
- 当前单文件 course.sqlite，可离线检索、回源并标明材料属性。
- 一批核心对照译文及审校记录，不承诺全手册翻译。
- 一份旧新检索对比，包含 no_match、omitted 和错误召回，不只报告命中率。
- 一条真实中文提问的完整示例：检索表达、命中原文、Agent 实际收到的内容、最终引用。
