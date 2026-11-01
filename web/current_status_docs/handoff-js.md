> 负责人：js工
> 类型：交接
> 更新：2026-11-01
> 状态：进行中
> 阶段：J6

# js工 · 交接

> ## ⚑ 文档规则有重大变更（2026-11-01）
> `current_status_docs/` 现在每人**三份**：`plan-<角色>.md`、`handoff-<角色>.md`、
> **`history-<角色>.md`（新增）**。`history` 是**模块实况**：实时改写、描述各模块当前实际行为，
> 不按计划或日期追加。请先读 README 的「文档规范」，并补一份 `history-cpp.md`（结构可参考 `history-js.md`）。

## ① 待对方配合

| ID | 事项 | 期望 |
|---|---|---|
| H-JS-002 | wave UI 需求 | 给出信号属性、时间精度、分页粒度，用于冻结 D10 |
| H-JS-003 | 静态数据字段 | 引脚表需要 `target-profiles/*.pins.json` 的哪些字段；与 cpp 校验共用同一份 |
| H-JS-004 | structure/lint 接口 | 按 [`contracts/analysis-v1`](../backend/cpp/contracts/analysis-v1/README.md) 输出；字段有变化提前说 |
| H-JS-005 | 文档规则变更 | 阅读 README「文档规范」，新增 `history-cpp.md` 并按实况维护 |

## ② 待定接口

| 编号 | 事项 | 冻结时机 |
|---|---|---|
| D10 | `wave serve` 请求/响应字段与数值编码 | P7 |
| D11 | 其余子命令（validate / capture / devices）的结果 payload | 各命令实现时 |

## ③ 已知风险

- 结构树/诊断 UI 已就绪但 C++ P6 未实现（当前显示"待实现"）；波形依赖 P7，Agent 依赖 bootstrap 对齐。
- 画布是原型：无缩放/平移/撤销，连线只做"输出→输入"，未做电气规则。
- HTTP 只有会话 Cookie：无 HTTPS、CSRF、登录限流；用户注册是开放的。
- 文件读写限文本扩展名与 2 MiB；二进制（位流、VCD）需走产物接口，尚未实现。
- 端到端验证全部使用假 yosys；真实工具链、板卡、Node↔真实 Yosys 均未联调。
- `config.txt` 的 `[wave]`/`[devices]` 键目前没有使用方，键名可能调整。

## ④ 最近变更

- 2026-11-01 文档规范变更：新增 `history-<角色>.md`（每人三份）；H-JS-004 契约移入 `contracts/analysis-v1/`。
- 2026-11-01 J5：画布（元件库、拖拽、引脚连线、删除、按工程保存）；11/11 测试通过。
- 2026-11-01 J4：CodeMirror 编辑器、结构树/诊断面板、`/structure` `/lint` 端点；10/10。
- 2026-11-01 J3：用户（scrypt）/会话/工程与文件；9/9。
- 2026-11-01 J2：HTTP/SSE + 产物下载 + 控制台；7/7。
- 2026-11-01 J1：`jobs/` + `util/`；5/5。
