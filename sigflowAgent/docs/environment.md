# 开发环境与依赖

复核日期：2026-10-08。工作目录：`SigFlow_Cmake/sigflowAgent`。

## 当前环境

| 项目 | 状态 |
|---|---|
| 操作系统 | Windows / PowerShell |
| 基础解释器 | `C:\Users\NiZhe\anaconda3\python.exe`，Python 3.13.5 |
| 项目解释器 | `sigflowAgent\.venv\Scripts\python.exe`，Python 3.13.5 |
| Conda | 不要求激活；已有 aiAgent 为 Python 3.10.20，不满足本项目 >=3.11 |
| SigFlow 基线 | `SigFlow_FPGA_CMake` / `b7e5186`，从 `3deee0e` 快进更新 |
| UCAgent 基线 | `ac00768`，从 `c96ceb1` 快进更新 |
| 上游源码位置 | `C:\Users\NiZhe\Desktop\SuperEDA\UCAgent\UCAgent` |

直接指定 `.venv` 的解释器可避免 PowerShell 激活脚本策略问题。基础解释器来自 Anaconda 不意味着需要启动 Conda 环境。

## 依赖层次

领域核心、规则 Provider、DeepSeek HTTP、Gateway HTTP 和上下文适配器均为标准库实现。运行核心测试无需安装第三方库，不需要本地 GPU、CUDA 或本地模型权重。

真实 UCAgent 类的导入依赖 LangChain、Pydantic、MCP、PyYAML、GitPython、psutil、Jinja2 及其传递依赖。本次仅安装阶段扩展 smoke 所需依赖，未安装完整 UCAgent 的全部 EDA、Web UI 或模型后端依赖。版本快照见 [requirements-ucagent-smoke.txt](../requirements-ucagent-smoke.txt)，针对当前 Windows / Python 3.13 环境生成，包含 `pywin32`；跨平台应另建并验证依赖锁定。

已验证组合：langchain 1.4.3、langchain-core 1.6.6、langchain-openai 1.6.7、langchain-mcp-adapters 0.3.2、mcp 1.30.0、pydantic 2.13.5。上游涉及私有 API，升级后须重跑集成测试。

## 重建环境

在 `sigflowAgent` 目录执行，基础 Python 需 >=3.11；当前复现基线为 3.13.5：

```powershell
python -m venv .venv
.\.venv\Scripts\python.exe -m pip install -r requirements-ucagent-smoke.txt
$env:SIGFLOW_UCAGENT_SOURCE = (Resolve-Path '..\..\UCAgent\UCAgent').Path
.\.venv\Scripts\python.exe -m pip check
.\.venv\Scripts\python.exe -m unittest discover -s tests -q
.\.venv\Scripts\python.exe -m sigflow_edu_agent.runtime.smoke
```

`SIGFLOW_UCAGENT_SOURCE` 指向包含 `ucagent/__init__.py` 的可信 checkout。测试与 smoke 显式加入此路径；库适配器本身不自动修改搜索路径。服务部署应通过安装固定上游版本或受控 `PYTHONPATH` 配置提供 `ucagent`。

当前测试从源码目录运行，无需安装本项目 wheel。`.venv` 和 `.env*` 已忽略；`.env.example` 保留为变量模板。凭据通过进程环境注入，不能写入依赖文件、文档、提交或测试日志。

## 联调前置条件

- DeepSeek：配置 `DEEPSEEK_API_KEY`、`DEEPSEEK_MODEL`，由账户实际可用模型决定模型 ID；本次未调用真实 API。
- Gateway：宿主提供端口、Bearer token 和预期 instance_id，运行于同机 127.0.0.1；不从无认证 health 自动接受新实例。
- EDA：本批不启动真实 Job；后续真实综合/仿真需由 SigFlow provider 配置工具链，不能用 Python smoke 替代工具链验收。
