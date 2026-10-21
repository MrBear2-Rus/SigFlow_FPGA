# SigFlow Edu Agent U1 runtime baseline

> 记录日期：2026-10-22
> 代码基线：`ea7117f`（启动器修复、路径脱敏、版本兼容矩阵与压力矩阵已提交；本次文档回填待提交）
> 范围：`agent/src/sigflow_edu_agent/`、sidecar bootstrap、接口 A 与契约测试。  
> 本文件只记录可复现事实；未运行的平台与项目明确标为未测。

## 1. 环境

| 项 | 实测值 |
|---|---|
| OS | Microsoft Windows NT 10.0.26200.0 |
| OS / process architecture | x64 / x64 |
| Python 主验收 | 3.13.16 |
| Python 补充验收 | 3.14.8 |
| Python 兼容矩阵 | 3.9.13 / 3.10.11 / 3.11.9 / 3.12.9 / 3.13.16 / 3.14.8，均 PASS |
| 运行时依赖 | 仅 Python 标准库；无第三方运行时依赖 |
| 服务监听 | `127.0.0.1`，端口由系统分配或 bootstrap 指定 |

## 2. 自动化验收

```text
python agent/tests/run_all.py
SUMMARY: run=306 failures=0 errors=0 skipped=1
```

```text
python -m unittest discover -s agent/tests -t agent
Ran 306 tests
OK (skipped=1)
```

跳过项是 Windows 无法捕获 SIGTERM 的平台差异；该平台使用鉴权后的
`POST /api/v1/shutdown` 验证正常退出。Windows 下的 SIGINT、鉴权 shutdown、
非鉴权拒绝和进程不自行退出均已测试。

零第三方导入检查：

```text
python -W error::DeprecationWarning -c "import sigflow_edu_agent"
import-ok
```

## 3. 已验收能力

| 项 | 结果 | 主要证据 |
|---|---|---|
| sidecar 自带测试 | PASS | 3.14.8 下两个测试入口均为 `306` 项、0 失败、0 错误 |
| bootstrap / readiness | PASS（Windows） | 单行 bootstrap/ready、nonce/协议、退出码、stdout 静默、信号与 shutdown |
| 接口 A 路由、鉴权、Host/Origin、限额 | PASS | `test_http_api.py`、`test_http_surface_boundaries.py` |
| 信封、错误码、ID、64 位值与限额 | PASS | `test_envelope_contract.py` |
| 四种 run、L4 拒绝、无 Key 降级 | PASS | `test_run_semantics.py`、`test_model_client.py` |
| 卡片构建与 PlanCard 校验 | PASS | `test_cards_contract.py` |
| 四条确定性 RTL 规则 | PASS | `test_rules.py` 正例、反例与合法锁存器样例 |
| 会话、run、事件存储 | PASS | `test_session_store.py`，含 32 LRU、64 run、1000 event、并发提交 |
| Gateway 唯一出站通道 | PASS | `test_gateway_client.py`，含禁代理/重定向、2 MiB、token 与错误映射 |
| 模型客户端与无 Key 降级 | PASS | `test_model_client.py`，模型回答失败关闭 |
| 契约与 fixtures | PASS | 25 份 schema 的运行期/共同 fixture 校验与三条历史偏差负例 |
| 限额、并发与压力矩阵 | PASS | `agent/tests/test_stress_matrix.py`、[stress-matrix.md](stress-matrix.md) |

### 3.1 历史偏差收口

以下三项已在当前代码和测试中收口，不再列为待修缺陷：

- `SourceRef.end_line` 最小为 1；空源码区间返回负例，不再生成 `end_line=0`。
- 波形 tick 同时接受十进制字符串和 uint64 JSON 整数，并拒绝越界、负数和非整数。
- Gateway 历史错误码 `PATH_OUTSIDE_PROJECT`、`SOURCE_UNAVAILABLE`、`PLUGIN_UNAVAILABLE`
  分别映射到 `POLICY_DENIED`、`GATEWAY_UNAVAILABLE`、`CAPABILITY_UNAVAILABLE`。


## 4. 平台矩阵

| 平台 | 结果 | 说明 |
|---|---|---|
| Windows x64 | PASS | 本次实测环境；全量 sidecar 测试通过 |
| Linux x64 | SKIP / 未测 | 当前没有 Linux 真机，未用 WSL/Docker 冒充 |
| 麒麟目标架构 | SKIP / 未测 | 未推定 ARM64/LoongArch 可用 |

双平台原生安装与发布验收**未完成**。本文件不把 Windows 结果视为 Linux
或麒麟发布的证明。

## 5. 已知限制

- sidecar 会话、run 与事件只在内存中，重启失效；不自动恢复旧会话或续跑。
- v1 没有课程库，`course_refs` 恒为空，`/capabilities.course.available=false`。
- 基础 `/runs` 的 L4 恒 403；没有 `hint_next`、`execute_plan` 或其它阶段动作路由。
- 接口 A events 立即返回，不支持长轮询或 `wait_ms`；Agent 没有 `/state`。
- 无模型 Key 时只提供确定性规则讲解；模型不可用不会伪造模型输出。
- 真实模型端点仍未验证：当前机器没有配置模型 Key，`/chat/completions` 成功往返记为
  `SKIP / 阻塞`；无 Key 降级和失败模式测试已通过。
- 当前 sidecar **没有 SQLite 实现**，不写 `data_directory`，也不持久化学习元数据或执行检查点。
  SQLite 学习记录和恢复检查点属于 B 阶段延期扩展，见 [b-stage-scope.md](b-stage-scope.md)。
- Linux x64 与麒麟仍未原生验收；Windows 结果不能外推为这两个平台已支持。

当前 B 阶段延期范围、归属和验收前置条件见
[b-stage-scope.md](b-stage-scope.md)。
