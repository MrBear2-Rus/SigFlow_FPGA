# Agent 模型与 API 仪表盘实施任务

依据 [设计文档](agent-model-dashboard-design.md) 与 [验证与交付记录](agent-model-dashboard-verification.md)。目标是在 Agent 页面提供一个宿主侧仪表盘，用于配置模型 API Key、查看模型状态、DeepSeek 余额与本机 token 用量。

| 批次 | 顺序 | 交付条件 | 状态 |
|---|---|---|---|
| 一 | DASH-01 → DASH-02 | sidecar 用量采集、两个只读接口、契约与 Python 测试 | 完成 |
| 二 | DASH-03 → DASH-04 | 宿主 DPAPI 存储、sidecar 重启、查询控制器与 C++ 测试 | 完成 |
| 三 | DASH-05 → DASH-06 | Agent 页面入口、仪表盘 UI、MainFrame 接线 | 完成 |
| 四 | DASH-07 | 实机验收、回归、文档整理 | 完成 |

## DASH-01 sidecar 用量与余额

- [x] 新增 `usage.py`：JSONL 追加、90 天裁剪、损坏行跳过、按日聚合、读最近有界尾部。
- [x] `ModelClient` 提取响应 `usage`；缺失/非法时标记 `tokens_unavailable`，不影响模型结果。
- [x] U2 `DeepSeekProvider` 通过可选 `usage_recorder` 写入同一份计数。
- [x] 新增 `fetch_balance()`：`GET /user/balance`、固定超时/错误码、不转发原始正文或 Key。

## DASH-02 接口与契约

- [x] `GET /api/v1/usage?days=30`（1–90，只读、不联网）。
- [x] `GET /api/v1/balance`（失败仍 200 + `available=false` 固定原因码）。
- [x] OpenAPI、`agent-usage.schema.json`、`agent-balance.schema.json`、golden fixture、CHANGELOG。
- [x] Python 测试：聚合/裁剪/坏行、余额成功与失败、路由鉴权、契约运行时 schema。

## DASH-03 宿主存储与重启

- [x] `AgentModelSettings`：字段校验、Windows 当前用户 DPAPI、原子写入、来源优先级、清除。
- [x] 非 Windows 返回 `unsupported_platform`，不提供明文回退。
- [x] `AgentServiceController::SetModelSettings()`；MainFrame 使用 `Stop → Set → Start` 使配置生效。
- [x] `AgentServiceController` 初始配置来源改为 `AgentModelSettings::Load`。

## DASH-04 查询控制器

- [x] `AgentApiClient::GetUsage/GetBalance`（10/15 秒有界）。
- [x] `AgentDashboardController`：单一后台任务顺序查询、请求序号、generation 快照。
- [x] C++ smoke：真实 loopback 响应、协议 schema 错误分支、DPAPI 往返与磁盘无明文。

## DASH-05 Agent 页面入口

- [x] `AgentWorkspace` 操作行新增“模型仪表盘”Ghost 按钮。
- [x] `SetDashboardHandler()` 宿主回调；工作区不读写配置、不依赖 MainFrame。
- [x] 窄宽度下与“选择报告”“返回工作台”不重叠。

## DASH-06 仪表盘 UI

- [x] 四个 `AgentCardHost` 区块：服务状态、API Key、账户余额、本机用量。
- [x] `AgentButton`（Primary/Secondary/Ghost）、Agent 字体与主题色、浅/深色订阅。
- [x] 内容区滚动；窗口缩放重绘；Key 不预填、保存后清空。
- [x] 重启/查询状态收口：Ready、失败、超时、未启用均有终态文案并停止轮询。

## DASH-07 实机验收与文档

- [x] 主程序构建：`cmake --build build-gcc --target sigflow -j 8`。
- [x] Python 定向测试 108 项通过；C++ 两个 smoke 通过。
- [x] 实机：打开 Agent 页面 → 模型仪表盘 → 保存并重启 → 余额/用量刷新；中文与缩放验收通过。
- [x] 更新设计、验证交付、任务文档与 `agent/README.md`、契约 CHANGELOG。

## 未纳入本轮

- GUI 夹具目标 `SIGFLOW_BUILD_AGENT_GUI_TESTS` 保持默认关闭；本轮界面验收通过实际启动、手动点击与程序化缩放完成。
- 历史用量回填、调用明细、费用估算、预算告警、CSV 导出。
- `Plugin_DeepSeek` 插件独立 Key 的统一管理。
