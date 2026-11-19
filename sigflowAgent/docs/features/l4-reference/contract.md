# L4 原生接口契约草案

契约标识为 `u2.native-reference.v1`。以下定义供 U1、U2、S3 实现和联合验收，现有部署尚未提供这些端点。[技术设计](design.md)

## 能力与认证

在 U2 既有 capabilities 内追加可选对象 `native_reference`，开启时含 `schema_version: "u2.native-reference.v1"` 和 `available: true`。能力代表通道已配置，当前运行是否可申请由专用状态返回。默认省略该对象，既有 `http_l4_available` 语义保持。GUI 将缺失、未知版本和不可用能力视为入口关闭。

专用端点沿用正式宿主的 Host、Origin 和普通 Bearer 边界。每个调用需要恰好一个 `X-Learner-ID` 和 `X-SigFlow-Reference-UI`。后一项为可信启动时为该 sidecar 生成的令牌，只传给原生控制器和 sidecar。缺失、重复或无效令牌拒绝。令牌不出现在能力值、响应、测试报告或模型上下文。通道关闭时专用端点返回 404。

## 路由和字段

完整路径前缀是 `/api/v1/u2/runs/{run_id}/reference`，其中 `run_id` 是对外运行标识，属于当前 `X-Learner-ID`。表内为追加到该前缀的后缀。输入拒绝未知字段、重复头和多余查询。版本和 epoch 均为十进制字符串，按现有有界整数规则解析。

| 方法与后缀 | JSON 请求 | 成功结果 |
| --- | --- | --- |
| GET，空后缀 | 无 | 200 状态对象 |
| POST `/request` | `action_id`、`expected_version` | 202 动作受理对象 |
| POST `/confirm` | `action_id`、`expected_version`、`challenge_id`、`receipt_id` | 202 动作受理对象 |
| POST `/body` | `expected_version` | 200 正文对象 |
| POST `/reconcile` | `action_id`、`expected_version` | 202 动作受理对象 |

沿用现有 HTTP 成功及失败 envelope。成功的 `data` 定义如下：

- 状态对象含 `schema_version`、外部 `session_id` 和 `run_id`、`state_version`、`status` 及可选的固定 `reason`。状态可为 `unavailable`、`eligible`、`awaiting_confirmation`、`pending`、`confirmed`、`reconciliation_required`。仅 `awaiting_confirmation` 返回挑战。
- 挑战含 `challenge_id`、`action_id`、`state_version`、`expires_at_epoch` 和 `receipt_scope`。签发范围含 `session_id`（U2 内部标识）、`project_id`、`revision`、`issue`、`policy` 和 `level`（固定为 `l4`）。界面可展示题目和修订，凭据签发使用完整绑定。
- 动作受理对象含 `schema_version`、外部 `session_id` 和 `run_id`、`accepted`、`pending`、`replay`。客户端从状态接口等待提交结果；202 只表示受理。确认必须使用挑战指定的 `action_id` 和状态版本。恢复使用新的外部恢复动作 ID 和当前版本，内部重核沿用原审批和核销键。
- 正文对象含 `schema_version`、外部 `session_id` 和 `run_id`、`state_version`、`reference_id`、`sha256`、`policy`、`revision` 和 `content`。正文是最多 32 KiB 的 UTF-8 纯文本。响应包含 `Cache-Control: no-store`。状态接口只返回状态或挑战，状态轮询与读取正文分开。

## 边界与失败

申请使用渲染时的状态版本。挑战发布后，签发和确认使用挑战内的发布版本。正文请求使用确认提交后的状态版本。服务端先核对授权、当前工程与绑定，再通过现有 U2 内部引用逻辑核销或读正文。读取完成后重新核对当前绑定和版本。原生端在显示前复核当前项目、修订、会话、运行及请求代次。

| 对外结果 | 使用场景 |
| --- | --- |
| 400 `INVALID_ARGUMENT` | 形状、版本格式、头重复或字段非法。 |
| 401 或 403，沿用现有认证错误 | 认证缺失或权限不允许。错误作用域、伪造凭据均为拒绝，不暴露其他运行是否存在。 |
| 404 `NOT_FOUND` | 通道关闭，或当前学习者没有可访问的运行。 |
| 409 `IDEMPOTENCY_CONFLICT` | 状态版本变更、待处理操作冲突，或动作键换了载荷。 |
| 429 `RESOURCE_EXHAUSTED` | 已有教学预算耗尽。 |
| 503 `SERVICE_UNAVAILABLE` | 可信配置依赖、Gateway 或持久化不可用。 |

已受理动作遇到挑战过期、撤销、材料变化等状态结果时，用专用状态的固定原因告知界面。两次确认之间发生修订变化，C++ 丢弃当前挑战，U2 复核并拒绝。服务端错误描述使用固定文案，不回显字段值、文件路径、材料正文或底层异常。

同一动作 ID 和载荷可重读元数据，重复下发不会获得第二次核销。清空界面时丢弃正文和迟到响应。Gateway UI 凭据签发只从可信 C++ 调用，且有效期最多 60 秒并受挑战剩余时长限制。签发响应丢失且尚未取得凭据 ID 时，C++ 等待当前挑战过期，由学生重新申请。读取正文始终重新验证当前授权，动作重放和状态查询继续只返回元数据。
