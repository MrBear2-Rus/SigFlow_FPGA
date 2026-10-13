# SigFlow Edu Agent：安装、升级与回滚

本文件是 `sigflow-edu-agent`（自研 Python sidecar）的发布材料。它只覆盖**服务包的安装与生命周期**；
Agent 与 SigFlow IDE 之间的进程启动边界见
[`docs/edu-agent/sigflow/sidecar-bootstrap.md`](../../docs/edu-agent/sigflow/sidecar-bootstrap.md)，
HTTP 契约见 [`contracts/edu-agent/v1/agent.openapi.yaml`](../../contracts/edu-agent/v1/agent.openapi.yaml)。

## 1. 交付内容与版本

| 项 | 值 |
|---|---|
| 包名 | `sigflow-edu-agent` |
| 版本 | `0.1.0`（`src/sigflow_edu_agent/version.py` 的 `AGENT_VERSION`） |
| 协议 | `edu.api.v1`（与 SigFlow Gateway 同主版本） |
| Python | >= 3.9（CI/实测：3.13.7 on Windows x64） |
| 运行时依赖 | **无**。只用标准库；`pyproject.toml` 的 `dependencies = []` 是刻意的 |
| 构建依赖 | `setuptools>=61`（仅打包时需要） |
| 许可证 | MIT，见 `LICENSE` |
| 平台 | Windows x64（已实测）、Linux x64（未实测，见第 6 节） |

**没有锁文件是设计结果**：零运行时依赖意味着没有需要锁定的第三方版本。如果将来引入依赖，
必须同时补 `requirements.lock` 与依赖审计，并更新本文件。

## 2. 安装

### 2.1 以 Python 包安装（推荐）

```text
python -m pip install ./agent
```

安装后得到：

- 控制台脚本 `sigflow-edu-agent`（来自 `[project.scripts]`）；
- 包 `sigflow_edu_agent`（src 布局，`agent/src/`）。

### 2.2 不安装、直接用源码树（开发/离线）

把 `agent/src` 放进 `PYTHONPATH` 即可，无需 pip：

```text
# Windows (cmd)
set PYTHONPATH=<repo>\agent\src
# POSIX
export PYTHONPATH=<repo>/agent/src
```

仓库自带的两个启动器 `agent/bin/sigflow-edu-agent.cmd` 与 `agent/bin/sigflow-edu-agent`
会自动把 `<启动器目录>/../src` 前置到 `PYTHONPATH`，因此**不需要**先 pip install。

### 2.3 交给 SigFlow 的入口

SigFlow 只认一个环境变量：`SIGFLOW_EDU_AGENT_EXECUTABLE` = 启动器的**绝对路径**。
SigFlow 会执行 `<该路径> --bootstrap-stdin`，并通过子进程 stdin 一次性传入 bootstrap JSON。

```text
# 例（Windows）
set SIGFLOW_EDU_AGENT_EXECUTABLE=E:\path\to\agent\bin\sigflow-edu-agent.cmd
```

启动器用 `%SIGFLOW_EDU_AGENT_PYTHON%`（Windows）/ `${SIGFLOW_EDU_AGENT_PYTHON:-python3}`（POSIX）
选择解释器；未设置时回退到 `python` / `python3`。**令牌从不进入命令行、URL、工程文件或环境变量。**

## 3. 验证安装

```text
python agent/scripts/verify_install.py
```

脚本只做本地只读检查（不联网、不启动服务）：包可导入、版本与协议正确、两个启动器存在、
`dependencies` 为空、模块清单齐全，并打印逐项 PASS/FAIL 与退出码。

端到端验证（可选，需要 Python 可用）：

```text
cd build-agent && ctest -R edu_agent_sidecar_smoke --output-on-failure
```

该测试会真的启动 sidecar，校验 bootstrap → ready（nonce/协议/端口/版本）→ `/health` →
无 Key 规则卡 → `/shutdown` 正常退出，并覆盖双实例隔离与**启动器入口**。

## 4. 升级

1. 记录当前版本：`python -c "import sigflow_edu_agent.version as v; print(v.AGENT_VERSION)"`。
2. 替换 `agent/` 目录或执行 `python -m pip install --upgrade ./agent`。
3. 重跑第 3 节的验证。
4. 重启 SigFlow：sidecar 由 IDE 管理，重启 IDE 即重启 sidecar。

兼容性：`edu.api.v1` 主版本内只新增可选字段；**协议主版本不匹配时 SigFlow 只禁用 Agent 功能**
（`AgentServiceController` 在 ready 校验阶段就拒绝），编辑、手动仿真与综合不受影响。

## 5. 回滚

- **pip 安装**：`python -m pip uninstall sigflow-edu-agent`，再装回上一版本目录。
- **源码树**：恢复上一版本代码；先备份 bootstrap 用户数据目录的 SQLite 数据，不假定旧版本理解新数据。
- **只停用不卸载**：清空 `SIGFLOW_EDU_AGENT_EXECUTABLE`（或删除该变量）后重启 SigFlow，
  sidecar 进入 `Disabled` 状态，IDE 原有功能照常。

本包**不写注册表、不装服务、不改系统 PATH**，因此回滚不需要清理系统状态。
它只在 bootstrap `data_directory` 下持久化最小学习元数据和执行恢复检查点；当前宿主用稳定的
用户数据路径 `AppData/sigflow-edu-agent/local/learning.sqlite3`，不是安装/工程目录。
学习记录默认不同意、保留 90 天；恢复日志不含源代码，独立于学习同意。卸载不自动删除用户数据，
需要删除时先导出/备份并确认。详见 [实现与验收](../docs/edu-agent/sigflow/10.10-Agent接口调整实现与验收.md)。

## 6. 已知边界与未验证项

- **Linux x64 未实测**：`agent/bin/sigflow-edu-agent` 是 POSIX sh 启动器（LF、`#!/bin/sh`），
  逻辑与 Windows 版一致，但**没有在 Linux 主机上运行过**。按项目约束，不能用 WSL 或 Docker
  代替原生验收，因此这里明确标注为未完成。
- **真实模型端点未实测**：未配置 `SIGFLOW_EDU_MODEL_API_KEY` 时服务降级为确定性规则讲解
  （`model.available=false`、`model_used=false`）。真实模型的往返调用未验证。
- **Docker 不是一期门槛**：容器/统一远端服务是按目标用户需求再评估的可选方案，
  不能替代原生 Windows/Linux 验收。
- **没有独立执行权限**：基础 `execution.can_run_eda=false` 保留；教学扩展只能拿 UI 签发、精确绑定计划/快照的 grant 请求 Gateway Job，不执行命令。
- **会话数据在内存中**：session/run 退出即丢失；最小学习元数据（显式同意后）及执行检查点由 SQLite 保存。恢复只查询事实，不自动续跑。
- **完整教学产品仍有 Gate**：目前 4 规则/4 自编材料，不替代 8 类问题/20 审校片段及真实模型、原生双平台和压力验收。
