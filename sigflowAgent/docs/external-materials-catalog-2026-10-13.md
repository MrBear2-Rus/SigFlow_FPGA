# 官方 HDL 资料引入实施记录

日期：2026-10-13

## 状态

- **已实现**：固定来源快照、hash 与行定位、结构切分、中文映射、核心对照译文、SQLite/FTS5 导入、Agent 上下文和引用回源。
- **开发验证通过**：新增资料专项 8 项通过；RAG/Agent 相关回归 49 项通过；最终数据库完整性、外键和正式 Agent 本地库烟测通过。
- **待审阅**：全部中文映射和 25 条机器译文；Verilator 生成 include 的文件级许可适用性；Yosys primer 的许可继承判断。
- **未验证**：本次未调用真实付费模型、未运行真实 EDA、未公开分发或推送 Git；完整测试中的 5 个 Gateway localhost 测试受沙箱网络限制，另有 6 个可选 UCAgent 集成测试跳过。少量检索测试不代表整体教学效果。

本记录是对 [实施计划](rag-external-materials-plan-2026-10-13.md) 的实际核对结果。当前运行入口继续是 `sigflow_edu_agent/rag/` 和单文件 `data/course.sqlite`，没有恢复旧活动 manifest、多版本库或兼容入口。

## 版本核对

资料版本与 SigFlow 工具版本分别登记。工具文档不能作为当前工程运行事实。

| 来源 | 知识库固定版本 | SigFlow 侧核对 | 本次实际运行版本 |
|---|---|---|---|
| lowRISC | `style-guides@9c15ff5dce23eef969e00ab3715153967419eadd` | 编码规范文档，不是 SigFlow 工具版本 | 不适用 |
| Yosys | 0.47，固定到 `647d61dd9212365a3cd44db219660b8f90b95cbd` | [脚本生成器](../../InnerPlugin/eda-synth-yosys/src/YosysScriptGenerator.cpp)标注随附 MinGW Yosys 0.49；[项目验收记录](../../docs/edu-agent/sigflow/task.md)也记录真实 Yosys 0.49。当前 `runtime/yosys/bin/yosys.exe` 存在 | 未运行 `--version`；本次遵守不运行真实 EDA 的要求，当前二进制版本未重新核实 |
| Verilator | v5.052，固定到 `ea338be98e1e838d3518809ce8899f85a009963c` | 项目记录曾以 v5.052 完成真实 `sim.build`；但本次检查 `external/fpga-tools/runtime/` 仅发现 apicula、nextpnr、openfpgaloader、yosys 目录，没有随仓库 Verilator 可执行文件 | 当前实际路径/版本未知；未运行工具 |

因此，Yosys 0.47 手册与项目记录中的 0.49 有版本差异；Verilator 文档固定为 5.052，但不能据此断言当前机器仍运行 5.052。每条外部材料保存独立 `course_version`，Agent 提示保留资料版本；要描述本次工程状态，必须引用当前工程报告或执行证据。

## 原文取得与库存

只取得计划清单及 warnings.rst 的直接 include，不抓取整站。所有固定清单目标均已取得：60 个原始文件，共 346,786 字节；54 个文件参与 passage 构建，另有 4 个许可证/README 证据文件和 2 个 L4 固定解答文件仅登记或排除。每个文件的仓库路径、固定 URL、commit/tag、SHA-256、字节数、行数、许可依据、审阅状态和本地文件创建时间见 [external_source_files.jsonl](../sigflow_edu_agent/rag/resources/courses/external_source_files.jsonl)。

| 来源 | 固定原始文件 | 原始字节 | 用于 passage 的源文件 | 官方原文 passage | 完整对照译文 |
|---|---:|---:|---:|---:|---:|
| lowRISC | 3 | 114,424 | 1 | 82 | 7 |
| Yosys 0.47 | 4 | 70,338 | 3 | 23 | 6 |
| Verilator v5.052 | 53 | 162,024 | 50 个来源文件中 48 个 gen 片段实际进入 passage；2 个 `_fixed.rst` 排除 | 162 | 12 |
| **合计** | **60** | **346,786** | **54** | **267** | **25** |

267 条原文 passage 合计 220,323 字符/字节；267 条中文检索映射为 91,842 字节，包含中文标题、英文原题/术语、现有 concept ID 与学生常见问法；25 条完整译文共 6,344 个 Unicode 字符、11,164 个 UTF-8 字节。映射和 passage 目录分别见 [mappings](../sigflow_edu_agent/rag/resources/courses/external_official_mappings.jsonl)、[passages](../sigflow_edu_agent/rag/resources/courses/external_official_passages.jsonl)、[spans](../sigflow_edu_agent/rag/resources/courses/external_official_spans.jsonl) 和 [translations](../sigflow_edu_agent/rag/resources/courses/external_official_translations.jsonl)。

`retrieved_at` 保留日期 `2026-10-13`；网络响应时间没有被采集。另记录的本地源文件创建时间范围为 `2026-10-13T12:56:14Z` 至 `2026-10-13T12:59:26Z`，这是文件系统证据，不冒充服务器下载时间。

固定入口：[lowRISC VerilogCodingStyle.md](https://github.com/lowRISC/style-guides/blob/9c15ff5dce23eef969e00ab3715153967419eadd/VerilogCodingStyle.md)、[Yosys 0.47 primer](https://github.com/YosysHQ/yosys/blob/647d61dd9212365a3cd44db219660b8f90b95cbd/docs/source/appendix/primer.rst)、[Verilator v5.052 warnings.rst](https://github.com/verilator/verilator/blob/ea338be98e1e838d3518809ce8899f85a009963c/docs/guide/warnings.rst)、[Verilator v5.052 languages.rst](https://github.com/verilator/verilator/blob/ea338be98e1e838d3518809ce8899f85a009963c/docs/guide/languages.rst)。Yosys `check` 和 `read_verilog` 帮助从固定提交的 [check.cc](https://github.com/YosysHQ/yosys/blob/647d61dd9212365a3cd44db219660b8f90b95cbd/passes/cmds/check.cc) 与 [verilog_frontend.cc](https://github.com/YosysHQ/yosys/blob/647d61dd9212365a3cd44db219660b8f90b95cbd/frontends/verilog/verilog_frontend.cc) 的帮助字符串规范化生成。

### 未取得或暂不纳入

- 固定清单中的文件没有缺项；本批没有扩展到计划以外的章节、外链论文、图片或商业工具资料。
- Yosys 社区中文手册未发现独立译文许可，且没有声明对应 Yosys 0.47；未复制其正文。降级方式是依据固定官方英文原文自建检索映射和对照译文。
- Verilator `warnings.rst` 引用的两个 `_fixed.rst` 文件虽在本地原始清单中校验 hash，但不进入 162 条 passage 和 Agent 上下文；它们是完整修复解答，按 L4 边界记录于 [external_exclusions.jsonl](../sigflow_edu_agent/rag/resources/courses/external_exclusions.jsonl)。
- warnings.rst 的 50 个 `docs/gen` include 中，48 个必要示例/诊断片段随对应 warning passage 保留自己的源文件和行范围；两个 fixed 示例排除。超出这些显式 include 的 ref 目标没有展开抓取。

## 许可与使用边界

| 文件范围 | 许可证据 | 登记判断与边界 |
|---|---|---|
| lowRISC 指南 | 固定提交的 [README](https://github.com/lowRISC/style-guides/blob/9c15ff5dce23eef969e00ab3715153967419eadd/README.md) 明确说明仓库文件采用 CC BY 4.0；保留来源和署名信息 | `explicit_repository_readme`；指南是 lowRISC 项目风格，不是通用功能正确性标准 |
| Yosys `check.cc`、`verilog_frontend.cc` 帮助字符串 | 两个固定 C++ 文件自身包含 ISC 授权及免责声明；仅提取/规范化帮助字符串 | `explicit_file_notice`；passage 的原始位置仍映射回 C++ 行范围 |
| Yosys primer | 固定 [COPYING](https://github.com/YosysHQ/yosys/blob/647d61dd9212365a3cd44db219660b8f90b95cbd/COPYING) 写明 ISC；primer 自身未见文件级许可头 | `repository_inherited_judgment`；属于仓库许可继承判断，待复核 |
| Verilator `warnings.rst`、`languages.rst` | 固定 [LICENSE](https://github.com/verilator/verilator/blob/ea338be98e1e838d3518809ce8899f85a009963c/LICENSE) 与两个文件内的 SPDX 声明均为 `LGPL-3.0-only OR Artistic-2.0` | `explicit_file_spdx`；完整来源、固定版本和行定位随材料登记 |
| Verilator `docs/gen/*.rst` | 50 个生成文件没有各自的许可声明；仓库 LICENSE 不能单独证明生成片段逐文件适用范围 | `repository_inherited_unconfirmed`；目前仅本地保留和本地检索，未公开分发，待许可复核 |

现有数据库还包含 YSYX 原文，其公开再分发许可仍未确认。外部资料许可明确不代表混合数据库可公开。原始源文件位于被忽略的 `data/external_sources/`；完整 passage、span 正文和译文资源不纳入 Python wheel 配置。当前只更新本地数据库，未 commit、push 或公开分发。

## 规范化、切分和定位

- lowRISC Markdown 按叶级标题切分；规则、示例和紧邻例外保持在同一语义片段内。
- Yosys primer 按 RST 标题层级的完整小节切分；`check` 和 `read_verilog` 各保留一条完整命令帮助。帮助文本由 C++ `log("...")` 字符串解码拼接，标记 `decoded_cpp_help_log_strings`，定位指向原 C++ 文件范围，不把实现源码冒充手册。
- Verilator warnings 按 `.. option::` 告警码切分，语言支持按完整叶级主题切分。include 路径只允许 warnings.rst 实际列出的 `ex_*.rst` 文件名，规范化到 pinned `docs/gen/`；含入 passage 的每个 span 都保留自己的 `source_path`、行号和 hash，不把示例行归到主文档。
- 每条 span 保存原始文件章节、行起止、GitHub 行锚点和片段 hash；规范化帮助另保存原始 C++ 行范围及源文本 hash。导入会重算固定原文 hash 并验证行范围。

准确范围示例：`verilator_5052:warnings:widthtrunc` 对应固定 [warnings.rst 第 2802–2806 行](https://github.com/verilator/verilator/blob/ea338be98e1e838d3518809ce8899f85a009963c/docs/guide/warnings.rst#L2802-L2806)，原文 5 行；原文件 SHA-256 为 `sha256:7fb2066f92359b7c0050ca1260a5dc793b5409905610d2c11f7769312b3b3afa`，passage 正文 hash 为 `sha256:5bc5049f30294693175fb3ded7393f7b36db5d87e620e70d1a3aefe2a9bb6b47`。中文译文 ID 为 `zh-CN:verilator_5052:warnings:widthtrunc:v1`，绑定同一原文 hash。

## 中文检索到 Agent 引用示例

可复现问题：**“为什么这里会有位宽告警？”**

1. 中文映射命中 `bit_width`，并关联英文原文；top-3 返回 `WIDTHEXPAND`、`WIDTHXZEXPAND`、`WIDTHTRUNC`。`language=zh-CN` 不会排除这些英文 passage。
2. `prepare_course_context()` 对三条完整 passage 各选择一份中文对照译文，不重复附上英文全文。上下文包含中文检索标题（明确标为仓库映射、非官方标题）、`translated_source` 属性、译文 ID、原文 hash、资料版本和原文行链接；未教师审阅状态明确可见。
3. `courses.pack_courses()` 保留工程证据优先和完整片段规则，最多接收三段、课程片段总预算 8192 UTF-8 字节；本例三个片段均进入上下文。整体请求仍由 DeepSeek 的原 16 KiB 护栏校验。
4. 本地 mock 模型候选引用 `verilator_5052:warnings:widthtrunc`，该 ID 在本轮 allow-list 内。引用核对再从 SQLite 读取来源，回到 `warnings.rst#L2802-L2806`；伪造 ID 会被拒绝。测试捕获了实际 Agent 请求体，没有调用真实模型。

如果问题只命中但材料超预算，`course_status` 会是 `partial` 或 `omitted`，`course_omitted` 保留 `byte_budget` 原因，未装入项不能被引用；检索没有证据才是 `no_match`。unsupported 概念继续返回 unsupported/no_match，不能用邻近主题补答。工具手册只能说明对应资料版本；lowRISC 风格不能独自判错当前工程。

## 数据库、测试与未完成项

最终 `data/course.sqlite` 为 schema v4，6,864,896 字节，`PRAGMA integrity_check=ok`、`foreign_key_check` 为空。数据库包含 533 条可检索正文：257 条 YSYX、9 条保持 `authored_note` 身份的旧讲解、267 条新增官方原文；仍登记 45 条不完整旧材料但不进入检索；9 条 HDLBits 练习保持独立。来源文件 provenance 为 60 条，映射 267 条，译文 25 条。当前内容标识为 `sha256:decf1b2993fa70585963c3788a6febc3b823c13916bedfe4a32697e34c54acb0`。

验证命令和结果：

| 命令/检查 | 结果 |
|---|---|
| `python -m sigflow_edu_agent.rag.external_materials` | 60 文件、54 构建源、267 passage、25 译文 |
| `python -m sigflow_edu_agent.rag.importer --snapshot-dir D:/EDA/doc/sources/ysyx/2607 --db data/external-build-temp.sqlite` | 临时库 v4 构建通过；测试期间未写运行库 |
| `python -m unittest tests.rag.test_course_kb tests.rag.test_agent_adapter tests.test_courses tests.test_external_materials -v` | 49 项通过 |
| `python -m unittest tests.test_external_materials -v` | 8 项通过 |
| `python -m unittest discover -s tests -v` | 109 项：98 通过、6 项可选 UCAgent 集成因环境未配置跳过、5 项 Gateway localhost 测试因沙箱阻断本地 HTTP 而失败（3 fail、2 error）；Gateway 与本次 RAG 改动无关 |
| 最终库 `integrity_check` / `foreign_key_check` | `ok` / 空；正式库 Agent 烟测通过 |

相关测试文件使用临时 SQLite 库；最终库仅在测试和完整性检查后通过正式 importer 构建。没有运行真实 EDA、真实付费模型或真实 Gateway 联调。

**待教师审阅**：25 条全文译稿的 must/should/may、条件和例外；267 条中文标题/术语/学生问法映射的概念覆盖；每类工具说明在 L1–L4 教学中的使用边界。**未完成教学效果验证**：没有真实课堂学生问法集、人工相关性标注、三种上下文配置的整体对比或教师答复充分性评测。不能据当前功能测试宣称总体命中率或教学质量已达标。
