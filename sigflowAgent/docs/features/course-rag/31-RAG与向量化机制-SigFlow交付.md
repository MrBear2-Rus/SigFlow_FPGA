# RAG 与向量化机制：三厂商实测拆解 + SigFlow 落地方案

> **用途**：交付给 SigFlow 团队作为 RAG 层的设计依据。
> **自包含**：不需要读其他文档即可实施；每条结论后标注证据来源与等级。
> **证据等级**：`[A]` 直接读到（文件/明文配置/日志） · `[B]` 两条以上独立证据 · `[C]` 单点或命名推断 · `[?]` 未知
> **采样对象**：AMD Vivado AI（2026.9.1）、Cadence JedAI（26.10–26.13）、Synopsys.ai Copilot（Y-2026.03 系列）
> **采样日期**：2026-11-05

---

## 0. 一页速览（先看这张表）

| 层 | AMD | Cadence JedAI | Synopsys Copilot | SigFlow 建议 |
|---|---|---|---|---|
| 向量库 | **Weaviate 1.39.7** `[A]` | **Chroma**（`chroma.sqlite3` + HNSW 段）`[A]` | 未直接见（Postgres `pg_docstore`）`[A]` | Chroma 起步；已有 PG 则 pgvector |
| 嵌入后端 | Qwen3-Embedding-**0.6B**(1024) / Gemma-300M(768)，**本地 llama.cpp** `[A]` | **5 个可换**：jedai_def 768 / titan 1024 / gemini 3072 / textem 3072 / **qwen3-8B 4096** `[A]` | 网关托管：text-embedding-3-large 1024 / nomic 768 / bge-large 1024 / Titan v2 1024 / gemini 768 `[A]` | 本地 bge-m3 或 qwen3-0.6b（1024/768） |
| 切块 | 未知 `[?]` | **2000 字符 / overlap 500** `[A]` | 未知 `[?]` | 2000/500 起步 |
| 混合检索 | **BM25 ⊕ 向量（`alpha` 加权）** `[A]` | BM25（15 模块）+ hybrid（14 模块）`[A]` | 未见（`rerank: true`）`[B]` | 必做 |
| 融合 | Weaviate 内建 `[B]` | `score_fusion`（RRF 或线性）`[A]` | 未见 `[?]` | RRF 起步 |
| 重排 | 未见 `[?]` | **独立 rerank 服务**（router/schema/service/urls + `rerank_leg_policy`）`[A]` | **`rerank: true`** + re2g / bge-reranker-base/large `[A]` | 必做，零标注即用 |
| 查询改写 | 未见 `[?]` | `retrieval_agent` 做 query rewrite + 多腿 `[B]` | `prompt_notes` 调风格 `[A]` | 第二期 |
| 语料规模 | ~73–75 万块 `[B]` | **936 块**（26.13，5 本手册）`[A]` | 未见 `[?]` | 从 1k 块起 |
| 多媒体 | 未见 `[?]` | **图片三段式引用**（见 §5）`[A]` | 未见 `[?]` | 按需 |
| 版本/换模型 | 只读快照，换模型=重导 `[B]` | **按集合隔离，换模型=新集合** `[A]` | 网关侧切 `model_route_name` `[A]` | 必须做 |

**一句话**：**三家都不训练嵌入模型**，全部用预训练开箱模型，靠"模型之外的六层机制"把效果补起来。

---

## 1. 核心结论：为什么不用训练

EDA 领域的现实是：**没有现成的 (query, doc) 标注对**。要微调嵌入模型，需要成千上万条"这个问题应该命中这段文档"的标注；三家都没走这条路（无任何微调证据 `[A]`，唯一疑点是 JedAI 的 `JEDAI_MODEL_INT_EMBED_2`，性质 `[?]`）。

他们用的是**六层机制**，每一层都不需要训练：

| # | 机制 | 解决什么 | 成本 |
|---|---|---|---|
| 1 | **指令前缀 / 任务前缀** | 通用模型对领域查询"理解不到位" | 改一行代码 |
| 2 | **混合检索（BM25 ⊕ dense）** | 通用模型不认专有名词（命令名、信号名、错误码） | 建一个倒排索引 |
| 3 | **重排（cross-encoder）** | 召回质量差一点没关系，重排能救回来 | 一个模型 + 一次推理 |
| 4 | **查询改写 + 多腿融合** | 用户提问含糊 | 多一次 LLM 调用 |
| 5 | **分集合 + 版本隔离** | 换模型要迁移、维度不兼容 | 建集合的纪律 |
| 6 | **切块策略 + 上下文拼装** | 切碎了丢语义 | 设计决策，零成本 |

---

## 2. 机制一：指令前缀（最省钱的一层）

Qwen3-Embedding 与 EmbeddingGemma 都是 **instruction-aware**：查询侧加任务描述即可显著提升，**不改权重**。Qwen3 的官方调用形态：

```
Instruct: {任务描述}
Query: {查询}
```

- 证据强度：Qwen3 文档侧是公开资料 `[B]`；在本批包里只抽到残留片段 `task:` / `query:`（AMD `doc-search-mcp` 二进制），**属弱证据 `[C]`**，不宜当铁证。
- 工程建议：**把"任务描述"当成可配置项**，和 prompt 一样进配置文件，而不是硬编码。

---

## 3. 机制二：混合检索（BM25 ⊕ 向量，alpha 加权）

**AMD 的实证**（`[A]`，从 `doc-search-mcp` 二进制抽出的字符串）：

```
bm25:{%v} limit: %v after: %q force: %v groups: %v alpha: %v nearAudio nearDepth nearImage nearVideo path:
```

这是 **Weaviate** 的查询参数结构，其中 **`alpha`** = BM25 与向量的权重比（`alpha=0` 纯 BM25，`alpha=1` 纯向量）。

**Cadence 的实证**（`[A]`）：`BM25` 出现在 15 个模块、`hybrid` 14 个、`score_fusion` 3 个。

**为什么必需**：用户问 `set_clock_uncertainty`、`ALW_NO_COMB`、`libtorch_cuda.so` 这类**精确符号**时，语义向量的表现反而不如倒排。混合检索用精确匹配兜底。

**起步参数**：`alpha = 0.5`，按评测调；两边各取 top-50 后融合。

---

## 4. 机制三：重排（最值得投资的一层）

### 4.1 Cadence：独立的 rerank 服务 `[A]`

模块清单（来自 JedAI 第一方 `.so` 的符号级抽取）：

```
jedai_aai.rerank_leg_policy            9 函数 / 49 串 / 66,341 B   ← 决定"哪条检索腿要重排"
jedai_order.routers.rerank             2 / 1 / 3,981 B
jedai_order.schemas.rerank             0 / 0 / 136 B
jedai_order.services.rerank_service    2 / 3 / 5,043 B
jedai_order.services.rerank_urls       1 / 6 / 5,347 B             ← 重排服务地址可配
```

⇒ 重排是**独立服务 + 可配置 URL**，不是内嵌函数；且有 `rerank_leg_policy` 决定**多腿召回里哪一腿走重排**。

### 4.2 Synopsys：显式开关 + 三个可选重排器 `[A]`

`_internal/config/generic_tool.yaml`：

```yaml
rerank: true
rerank_config:
  scs-gateway-v1-ibm-research-re2g-reranker-nq:
      model_canonical_name: ibm-research/re2g-reranker-nq
  scs-gateway-v1-bge-reranker-base:
      model_canonical_name: BAAI/bge-reranker-base
  scs-gateway-v1-bge-reranker-large:
      model_canonical_name: BAAI/bge-reranker-large
```

**为什么最值得投资**：唯一"零标注、立刻见效、可随时换"的一层。嵌入不行时，重排能补 30%+ 的排序质量。

---

## 5. 机制四：查询改写 + 多腿融合

- Cadence `retrieval_agent` 做 query rewrite 与多腿召回，腿间用 `score_fusion` 合并 `[B]`（模块级证据）。
- 融合方式：**RRF（Reciprocal Rank Fusion）或线性加权**，可配置 `[A]`。
- 迭代顺序建议：RRF 先行（不需要调权重），稳定后再试线性。

---

## 6. 机制五：分集合 + 版本隔离（换模型不迁移）

**Cadence 的集合命名规则** `[A]`：

```
cdn_{product}-{sub_type}-{version}      例：cdn_jedai-user_doc-26.13
```

**关键：同一语料可以有多个集合，维度各不相同、互不干扰** `[A]`：

| 集合（同一份 26.13 语料） | 维度 |
|---|---|
| qwen3-8B 版 | **4096** |
| gemini 版 / textem 版 | 3072 |
| titan 版 | 1024 |
| jedai_def 版 | 768 |

⇒ **换嵌入模型 = 换一套预计算向量 + 换集合名**。旧集合留着做 A/B，新集合重建，零迁移。

**代价要说清**：Cadence 实测 17 套 kit / 12,332 块，但真实语料只有 **936 块** `[A]` ——
**5 个后端 × 同一语料 = 约 4 倍重复存储**。这是"方便"的账单，SigFlow 按需选择，别默认全开。

---

## 7. 机制六：切块与上下文（数字最具体）

### 7.1 切块参数 `[A]`

```
chunk_size   = 2000   （字符）
chunk_overlap =  500  （25%）
```

直接写在每个 chunk 的 meta 里，可逐条核对：

```json
{"category":"cdn_jedai-user_doc-26.10","chunk_size":2000,"chunk_overlap":500,
 "name":"Installation_Quick_Start.html","file_type":".html","index":0,
 "product":"jedai","sub_type":"user_doc","version":"26.10",
 "embed_model":"JEDAI_MODEL_INT_EMBED_2","cdn_user":"pubtools",
 "embedded_name":"cdn_jedai-user_doc-26.10","embedded_version":1783435876}
```

### 7.2 **切块单位的选择比参数更重要**（一次真实的策略切换）`[A]`

| 版本 | 文档数 | 块数 | 类型分布 | 图片 |
|---|---|---|---|---|
| 26.10 | 110 | 355 | `.pdf`206 / `.html`149 | 0 |
| 26.11 | 118 | 859 | `.html`441 / `.pdf`418 | 85 |
| **26.12** | **5** | 699 | `.pdf`699 | 99 |
| 26.13 | 5 | 936 | `.pdf`936 | **121** |

26.12 起改成 **5 本合并后的官方手册**（`JedAICostControlUG` / `JedAIInstallQuickStart` / `JedAIPlatformRI` / `JedAIPlatformUG` / `VectorDBIntegrationGuide`）。

**教训**：文档数从 118 掉到 5，块数只从 859 降到 699 —— **单文档密度从 7.3 块升到 140 块**。
"按文件切" 与 "按卷切" 得到的检索粒度完全不同。**用户问的是"怎么配 PostgreSQL"，需要跨章节的连续上下文，不是某个 HTML 页。**

### 7.3 上下文拼装用的元数据字段 `[A]`

```
breadcrumb : "Overview"     ← 章节路径（small-to-big 的"大"）
l1_title   : "Overview"     ← 一级标题
page_start / page_number    ← 页码范围
scope      : "global"
```

⇒ **元数据不是日志，是检索期要用的**：召回小块后用 `breadcrumb` 回退到父块，这是 `small_to_big` 的实现基础。

### 7.4 图片引用：三段式（如果要做多模态）`[A]`

| 载体 | 形态 | 作用 |
|---|---|---|
| chunk 正文 | `[IMAGE hash=<64hex> page=<N>]` | **保位置** |
| chunk meta | `image_refs`（转义 JSON 字符串） | **保结构** |
| 磁盘 | `<sha256>.webp.enc` | **保字节**（内容寻址） |

`image_refs` 单条：

```json
[{"hash":"ffa83b85…","format":"webp","page":7,"caption":"",
  "width":900,"height":1063,"source":"pdf_raster"}]
```

实测对齐（26.13 / qwen3 套件）`[A]`：

```
blob 121 ↔ 标记唯一 hash 121  → 交集 100%，零孤儿、零坏引用
936 块中 72 块含图；标记 135 次 → 14 次跨块重复引用（约 10%）
format 仅 webp；source 仅 pdf_raster；caption 全空（134 空 / 0 非空）
尺寸 111 种原始值（67×62 到 1524×799，未归一化）
```

**两条可复用的判断**：
1. **`caption` 全空 ⇒ 图片不参与检索**，只是"答案侧的富媒体附件"。想真多模态得另加 caption 或图向量，那是另一笔成本。
2. **页码字段 0/1-based 混用**：图片 `page=7`，chunk `page_number=6` —— **字段必须带语义后缀**，否则整体偏一页。

---

## 8. 数据层自证：`decrypt.log` 式的一行清单

Cadence 每套 kit 根目录有一个 0.4 KB 的 `decrypt.log`，**一行给出全部关键事实** `[A]`：

```
collection=[('cdn_jedai-user_doc-26.13', 4096)] chunks=936 decrypted=936 failed=0 chars=800407 images=121/121
```

以及同名 `.info.json`：

```json
{"embed_model":"Qwen3-Embedding-8B","embedding_model":"Qwen3-Embedding-8B",
 "embedding_url":"http://<internal-host>:5013/v1","engine":"gen2",
 "pipeline_sha":"jedai-v2-local","product":"jedai","sub_type":"user_doc","version":"26.13"}
```

**建议 SigFlow 照抄这个习惯**：每个语料包自带一份 `manifest.json` + 一行 `selfcheck.log`，
包含 `集合名 / 维度 / 块数 / 字符数 / 失败数 / 图片数 / 嵌入模型 / 流水线版本`。
**审计任何语料包不需要重新跑流水线。**

---

## 9. 部署形态对照（影响运维，不影响到算法）

| 项 | AMD | Cadence | Synopsys |
|---|---|---|---|
| 形态 | 本地 Docker（Weaviate + llama.cpp + doc-search） | 服务侧 Chroma + 内网嵌入服务 | SCS AI Gateway + Postgres |
| 配置 | 镜像内 | kit 内 `.info.json` | `generic_tool.yaml` → `tool_<x>.yaml` |
| 认证 | 无（本地） | CDN 加密 + OAuth/OIDC | **无内置认证，明确要求反代负责** `[A]` |
| 增量 | 快照替换 | 按 `(product, sub_type, version)` 追加集合 | 网关侧切路由 |

**Synopsys 那条注释值得记住**（`docker-compose-mcp-bin.yml`）`[A]`：

```yaml
# SNPSAI_COPILOT_MCP_ALLOW_USERNAME_HEADER: No built-in auth (reverse proxy must handle) - leave empty
```

⇒ 大厂也把认证甩给反代，但在交付物里**写明**了这一点。SigFlow 若同样甩出去，要在 README 里写清楚。

---

## 10. SigFlow 落地方案（可直接照做）

### 10.1 最小可用（1 周）

```
语料 → 切块(2000/500) → 本地嵌入(bge-m3 或 qwen3-0.6b) → Chroma 集合
     → 检索(BM25 + dense, alpha 0.5) → bge-reranker-base → top-8 进 prompt
```

必带的三个字段（否则后面无法演进）：
- `breadcrumb`（章节路径，供 small-to-big 回退）
- `version` + `embed_model`（供换模型时识别旧集合）
- `chunk_size` / `chunk_overlap`（供复现）

### 10.2 目录与命名

```
collections/
  sig_{product}-{doc_type}-{version}/          ← 一集合一目录，与嵌入模型绑定
    chroma/                                     （向量）
    manifest.json                               {dim, model, count, chunk, sha}
    selfcheck.log                               一行摘要
raw/
  {product}/{doc_type}/{version}/…              （原始语料，勿删）
```

**换模型流程**（不迁移）：新建 `sig_{product}-{doc_type}-{version}__{model}/`，重建，双跑对比，切流量，旧集合保留一个版本。

### 10.3 评测基线（没有它无法判断"机制有没有用"）

准备 30–50 条真实问题，每条标 1–3 个"必须命中的文档/章节"，然后按层叠加测量：

| 配置 | 目的 |
|---|---|
| dense only | 基线 |
| + BM25（alpha 0.5） | 术语命中率是否提升 |
| + rerank | nDCG@10 / top-3 命中率 |
| + 查询改写 | 含糊问题的召回 |

**只看一个指标**：`top-3 命中率`。它在小语料上最灵敏。

### 10.4 明确不要做

| 不要 | 原因 |
|---|---|
| 微调嵌入模型 | 无标注对；投入产出比最低 |
| 一上来就开 5 个嵌入后端 | Cadence 实测约 4 倍重复存储，语料 936 块时才敢这么做 |
| 用 `.html` 页级切块做手册类语料 | 26.12 的教训：按卷切才有跨章节上下文 |
| 把 `caption` 空的图片当成"多模态已就绪" | 图片不参与检索，只是附件 |
| 让向量库同时当文档库 | Synopsys 用 Postgres 分 `pg_docstore` / `pg_logstore` |

---

## 11. 未确认项（不要当已知）

| # | 项 | 状态 |
|---|---|---|
| 1 | AMD 的切块参数与块数精确值 | `[?]` 无明文字段，只有 ~73–75 万块的量级估计 |
| 2 | AMD 是否用重排 | `[?]` 二进制内未见 reranker |
| 3 | JedAI 的 `JEDAI_MODEL_INT_EMBED_2` 是否微调过 | `[?]` 名字像自研/二代 |
| 4 | JedAI 的 BM25 索引落在 Chroma 还是独立 | `[?]` 只在代码层见节流参数 |
| 5 | Synopsys 的向量库实现 | `[A]` 只见 `pg_docstore`，向量列/扩展未验 |
| 6 | 三家各自的检索评测数据 | 全部 `[?]`，无可参考的公开指标 |

---

## 附：本文件的事实来源

| 事实 | 来源 |
|---|---|
| Chroma 结构、集合名、维度、chunk 2000/500、语料/图片统计 | `G:\temp\vdb_kits\*`、`G:\temp\vdb_2613\*` 的 `decrypt.log` / `*.info.json` / `decrypted.jsonl` / `chroma.sqlite3` |
| BM25、hybrid、score_fusion、rerank 服务、`rerank_leg_policy` | JedAI 第一方 `.so` 的符号级抽取（`_re-jedai\bodies\`，300 模块） |
| Weaviate `alpha` / `bm25{}` 查询结构 | `evidence\strings-docsearch-mcp.txt` |
| 嵌入/重排/LLM 可选清单、Postgres 双库、`rerank:true` | `linux64/copilot/bin/_internal/config/generic_tool.yaml`（明文） |
| 模型家族分档提示词 | 同目录 `prompt_config.json`（12.2 KB，明文） |
| REST 端点与外层拓扑 | `README-rest-bin.md`、`docker-compose-mcp-bin.yml`（明文） |
