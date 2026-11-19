# sigflowAgent · U2 教学主控

本模块维护 U2 教学主循环、会话宿主、工程证据适配、课程与学习记录接入，以及 DeepSeek 卡片生成。正式桌面端通过 U1 Sidecar 调用教学扩展，U2 也提供独立的开发和调试入口。

## 开发入口

需要 Python 3.11+。以下命令在仓库的 `sigflowAgent` 目录执行，可直接调用虚拟环境解释器。

```powershell
python -m venv .venv
.\.venv\Scripts\python.exe -m sigflow_teaching_core --provider rule --once
.\.venv\Scripts\python.exe -m sigflow_teaching_core.evaluation --suite tests/evaluation/development.json
```

规则模式与开发评测使用示例证据，无需 DeepSeek 密钥。课程检索默认读取仓库中的 `data/course.sqlite`。正式宿主、UA-10 和历史测试的运行范围见[环境指南](docs/guides/environment.md)，课程构建见[课程库说明](docs/guides/rag-integration.md)。

## DeepSeek

以下命令在仓库根目录执行。Windows 本机配置以当前用户身份加密保存，`test` 使用内置示例发送一次 API 请求。

```powershell
.\sigflowAgent\bin\sigflow-deepseek.cmd configure --model deepseek-flash
.\sigflowAgent\bin\sigflow-deepseek.cmd status
.\sigflowAgent\bin\sigflow-deepseek.cmd test
```

环境变量、加密配置位置和调用边界见 [DeepSeek 配置](docs/guides/deepseek-local-config.md)。

## 结构与文档

| 位置 | 职责 |
| --- | --- |
| `sigflow_teaching_core/` | 主循环、教学策略、宿主状态和工程证据适配 |
| `ua10_module/` | 学习事实存储、去重和历史查询 |
| `tests/`、`integration/` | 核心回归及可选真实 EDA 联调 |
| `docs/` | 开发指南、接口、功能专题、验收与历史记录 |

从[开发文档](docs/README.md)查找入口。GUI 联调见[接口索引](docs/interfaces/README.md)，交付和手工签收边界见[U2 状态](docs/status.md)。
