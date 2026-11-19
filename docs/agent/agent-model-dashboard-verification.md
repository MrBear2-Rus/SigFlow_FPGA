# Agent 模型与 API 仪表盘验证与交付记录

> 版本：v1.0 · 2026-11-17。状态：已实施并验收。
> 基线：2026-11-17 工作区（改动未提交）。
> 环境：Windows 11、MinGW-w64 GCC 16.2.0、wxWidgets 3.2.11（`3rd/wx-mingw32`）、CMake + `mingw32-make`。
> 设计：[agent-model-dashboard-design.md](agent-model-dashboard-design.md)
> 执行清单：[agent-model-dashboard-task.md](agent-model-dashboard-task.md)

## 1. 实施记录

### 新增/修改文件

| 文件 | 内容 |
|---|---|
| `agent/src/sigflow_edu_agent/usage.py`（新增） | 本地 JSONL 计数、90 天裁剪、坏行容错、按日聚合 |
| `agent/src/sigflow_edu_agent/model_client.py` | 提取 `usage` 并记录；新增 `fetch_balance()` 与固定原因码 |
| `sigflowAgent/sigflow_teaching_core/deepseek.py` | U2 教学模型调用同样记录 `usage`，录制失败不影响模型结果 |
| `sigflowAgent/sigflow_teaching_core/host/factory.py` | `make_factory(..., usage_recorder=...)` |
| `agent/src/sigflow_edu_agent/teaching_factory.py` | 把同一个 `UsageStore` 注入 U2 模型提供方 |
| `agent/src/sigflow_edu_agent/http_server.py` | `GET /api/v1/usage`、`GET /api/v1/balance`；沿用 UI token |
| `agent/src/sigflow_edu_agent/__main__.py` | 创建 `UsageStore` 并注入 `ModelClient`/`ServerConfig` |
| `contracts/edu-agent/v1/agent.openapi.yaml` | 两个新路径与 `UsageEnvelope`/`BalanceEnvelope` |
| `contracts/edu-agent/v1/schemas/agent-usage.schema.json`（新增） | usage data 字段真源 |
| `contracts/edu-agent/v1/schemas/agent-balance.schema.json`（新增） | balance data 字段真源 |
| `contracts/edu-agent/v1/fixtures/golden.json` | 两组 golden fixture |
| `main/agent/AgentModelSettings.{h,cpp}`（新增） | 校验、Windows DPAPI、原子写入、来源优先级 |
| `main/agent/AgentServiceController.{h,cpp}` | `SetModelSettings()`（Stop→Set→Start 生效） |
| `main/agent/AgentDashboardController.{h,cpp}`（新增） | 后台 usage+balance 查询与快照回调 |
| `main/agent/gui/AgentDashboardDialog.{h,cpp}`（新增） | 服务状态、Key、余额、本机用量四块 UI；滚动内容区、Agent 卡片/按钮主题、重启与查询状态收口 |
| `main/agent/gui/AgentWorkspace.{h,cpp}` | 操作行“模型仪表盘”按钮 + 宿主回调 |
| `main/agent/gui/AgentControls.cpp` | `AgentCardHost::Arrange()` 移动/缩放内容后重绘，消除拖拽重影 |
| `main/MainFrame.{h,cpp}` | 初始配置加载、仪表盘接线、sidecar 重启 |
| `tests/agent/edu_model_settings_smoke.cpp`（新增） | DPAPI 往返、优先级、损坏/清除 |
| `tests/agent/edu_dashboard_controller_smoke.cpp`（新增） | 真实 loopback 查询与协议错误分支 |
| `agent/tests/test_usage.py`、`test_model_usage.py`、`test_usage_http.py`（新增） | sidecar 用量/余额/路由测试 |
| `agent/tests/test_teaching_usage.py`（新增） | U2 教学模型调用的用量录制与失败不掩盖 |
| `agent/README.md` | 路由表补充两个只读接口 |

### 验证命令

```bash
# sidecar 定向测试
cd agent
PYTHONPATH=src python -m unittest tests.test_usage tests.test_model_usage tests.test_usage_http \
  tests.test_teaching_usage \
  tests.test_http_surface_boundaries tests.test_http_api \
  tests.test_contract_consistency.RuntimeAgentSchemaTests

# C++ 存储与控制器
cmake --build build-gcc --target edu_model_settings_smoke edu_dashboard_controller_smoke
cd build-gcc
ctest -R "edu_model_settings_smoke|edu_dashboard_controller_smoke" --output-on-failure
cd ..

# 主程序构建
cmake --build build-gcc --target sigflow -j 8
```

已知与本功能无关的既有测试失败（在 HEAD 同样失败）：`SafeTeachingAnswerTests` 的代码标记策略用例、`FixtureSchemaTests` 的 U2 schema 覆盖、`test_bootstrap_process` 的 Gateway 不可达用例、`test_u2_compatibility` 的可选模块跳过用例。

## 2. 交付结果

### 2.1 交付内容

1. Agent 页面操作行新增“模型仪表盘”入口（`AgentButton::Ghost`），宿主回调接入 `MainFrame`。
2. API Key 本机配置：Windows 当前用户 DPAPI 加密、原子写入、环境变量优先级、校验与清除；保存后自动重启 sidecar 生效。
3. 模型状态、Key 来源、DeepSeek 账户余额、本机 token 用量（累计 + 按日）四个区块；余额与用量都是只读查询。
4. sidecar 新增 `GET /api/v1/usage`、`GET /api/v1/balance`；OpenAPI、JSON Schema、golden fixture、契约测试同步更新。
5. 基础 `ModelClient` 与 U2 `DeepSeekProvider` 的模型调用都写入同一份本地用量记录。
6. 本设计文档、`agent/README.md` 路由表和 `contracts/edu-agent/v1/CHANGELOG.md`。

### 2.2 验收证据

| 项目 | 结果 |
|---|---|
| 主程序构建 | `cmake --build build-gcc --target sigflow -j 8` 成功 |
| Python 定向测试 | 108 项通过（usage/model/balance/teaching usage/HTTP 路由/契约运行时 schema） |
| C++ 测试 | `edu_model_settings_smoke`、`edu_dashboard_controller_smoke` 通过 |
| 全量 sidecar 套件 | 25 failures / 1 error，与 HEAD 基线一致，均为既有问题 |
| 实际界面 | Agent 页面显示“模型仪表盘”；对话框正常打开，中文无乱码，主题与 Agent 页面一致 |
| 保存并重启 | 实测 sidecar 端口从 49286 切换到 56165，状态回到 ready；说明新 Key 已通过 bootstrap 生效 |
| 余额/用量 | 实测余额 54.38 CNY；底部状态显示“用量与余额已更新。” |
| 重绘 | 用户手动验收无重影；另以程序化放大/缩小截图复核，未发现文字或边框残留；内容区可滚动 |

### 2.3 验收期间发现并修复的问题

| 问题 | 根因 | 修复 |
|---|---|---|
| 点击“模型仪表盘”进程 SIGSEGV | `wxString::Format("Agent 服务：%s", const char*)` 在 wx 3.2.11 + MinGW 下格式串宽转换崩溃 | `AgentServiceSummary()` 改为字符串拼接 |
| 对话框中文乱码 | 窄 `const char*` 中文字面量按系统 ANSI 代码页解释 | 新增 `U8()`/`wxString::FromUTF8`，覆盖对话框与状态文案 |
| 界面与 Agent 页面不统一 | 使用原生 wxDialog/wxButton/wxStaticBoxSizer | 改用 `AgentCardHost`、`AgentButton`、Agent 排版 token 与主题订阅 |
| “正在重启”提示长期停留 | `m_keyNote` 保存后从未更新 | Ready/失败/超时/未启用分别更新为终态文案并停止轮询 |
| 拖拽窗口出现 UI 重影 | owner-draw 卡片在子内容移动后不重绘；窗口缩小时内容被裁剪 | `AgentCardHost::Arrange()` 重绘、对话框 `OnSize` 刷新、内容区改为 `wxScrolledWindow`、避免无变化的重复 Wrap/Layout |

### 2.4 使用入口

1. 打开任意工程或无工程主界面。
2. `Help → Teaching Agent Workspace`。
3. Agent 页面操作行点击“模型仪表盘”。
4. 输入 API Key（留空表示保留已保存的 Key），点击“保存并重启”。
5. 重启完成后查看服务状态、账户余额与本机用量；窗口变矮时滚动内容区。

### 2.5 已知限制

- 非 Windows 不提供本机密钥库，仅支持 `SIGFLOW_EDU_MODEL_API_KEY` 等环境变量。
- 本机用量从本版本上线后开始记录，无历史回填；DeepSeek 不提供公开的历史用量 API。
- 余额查询仅支持 `api.deepseek.com`；其他提供方返回 `balance_unsupported_provider`。
- 不做调用明细、费用估算、预算告警、CSV 导出；不管理 `Plugin_DeepSeek` 插件的独立 Key。
- GUI 夹具目标 `SIGFLOW_BUILD_AGENT_GUI_TESTS` 默认关闭；本轮以实际启动、手动点击与程序化缩放截图完成界面验收。

### 2.6 交付状态

- 所有改动保留在工作区，**未提交**（仓库约定：提交/推送需用户明确要求）。
- 用户原有的 `3rd/httplib/httplib.h`、`tools/*` 修改未触碰。
- 复现构建：

```bash
cmake -S . -B build-gcc
cmake --build build-gcc --target sigflow -j 8
```

验证前需关闭正在运行的 `sigflow.exe`（Windows 会锁定输出文件）。
