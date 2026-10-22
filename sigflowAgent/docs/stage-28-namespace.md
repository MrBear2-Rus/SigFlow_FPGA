# 阶段 28：U2 导入与安装隔离

日期：2026-10-20。范围仅为命名空间、分发和入口兼容；共享教学卡片接入另见阶段 28 适配器文档。

## 正式宿主入口

U1 继续导入 `sigflow_edu_agent`，U2 使用 `sigflow_u2`。例如：

```python
from sigflow_u2.host.runtime import Runtime
```

U2 的安装分发名为 `sigflow-u2-teaching`，Python >=3.11，运行时无需第三方依赖。wheel 仅包含 `sigflow_u2` 实现和相应的课程资源，不安装 `sigflow_edu_agent`，不覆盖 U1 包。三个原有脚本名保持为 `sigflow-agent`、`sigflow-u2-host`、`sigflow-deepseek`，目标改为新命名空间。它们与 U1 的 `sigflow-edu-agent` 脚本名不同。

源码目录内的 `sigflow_u2/__init__.py` 将自己的包搜索路径指向同目录下既有实现，完全不导入、替换或设置 `sys.modules['sigflow_edu_agent']`。wheel 则用 setuptools 的 `package-dir` 映射直接安装同一份实现；不会把源码桥接文件作为安装后的包入口。因此不复制整套实现，也不要求加载 U1 后再清理模块缓存。

`host.closed_loop` 的子进程入口使用当前 `__package__`：通过 `sigflow_u2` 加载时启动 `sigflow_u2.host`，通过原源码入口加载时仍启动 `sigflow_edu_agent.host`。

## 开发兼容与限制

从 `sigflowAgent` 目录可继续运行 `python -m sigflow_edu_agent --demo`，也可运行 `python -m sigflow_u2 --demo`。正式宿主应显式导入新名称，并让 U1 自身的源码/安装路径负责旧名称的解析。

旧 U2 名称仅保留源码开发兼容；新版 wheel 不再提供旧 U2 导入名称。在同时存在两个旧名称源码根目录时，`sigflow_edu_agent` 仍由 Python 搜索路径顺序决定归属。因此不要用旧 U2 名称接入正式宿主，也不要在同一会话中混用新旧 U2 类实例并依赖其类型身份相等。

历史 U2 与 U1 同名分发的已安装环境，需要按各自安装来源核对迁移；不要假定卸载 `sigflow-edu-agent` 仅会移除 U2。本阶段未修改、卸载或重装系统环境中的任何包。

setuptools 的包清单按现有带 `__init__.py` 的目录显式声明。新增子包时需同步维护清单。课程资源的既有包含和排除规则保持有效；`course-build` 可选额外依赖只用于显式课程构建。

## 验证方法

`tests/test_namespace_packaging.py` 使用标准库 `unittest`，在工作区 `.superpowers/sdd/u2-stage28-31` 下创建和清理各自的临时目录，不读取真实密钥，不调用 API。子进程不继承模型配置，明确使用不存在的配置文件，禁用 site 加载验证导入隔离。

验证包括 U1 真实源码包与 U2 的两种导入顺序、新旧源码 CLI 的规则演示、新命名空间启动本身的 sidecar，以及离线构建、检查、安装 wheel 后的共存导入、课程资源、脚本目标和规则演示。wheel 在源码副本中构建，以免生成源码目录下的 build/egg-info。

可用已存在的 pip/setuptools/wheel 运行：

```text
python -m unittest discover -s sigflowAgent/tests -p test_namespace_packaging.py -v
```

构建与安装都显式使用 `--no-index --no-deps`，构建另加 `--no-build-isolation`。不存在已有构建工具时，仅 wheel 测试明确跳过，其余运行时隔离测试继续执行；本次使用现有 Anaconda Python 3.13 构建验证，不下载依赖。

红绿记录位于 `.superpowers/sdd/u2-stage28-31/namespace-red.log` 和 `namespace-green.log`。RED 确认旧实现无法导入 `sigflow_u2`，且构建产物仍使用冲突分发名；实现后重新运行同一测试集合。

本次实测：新增 4 项测试全部通过（含实际离线构建和安装）；原 CLI/closed-loop 定向回归 16 项全部通过，记录在 `namespace-compat.log`。`git diff --check` 对本阶段涉及文件通过。未执行模型批次、真实 Core 工具验收或系统环境安装。

2026-10-21 最终源码重新离线构建和安装：同一 4 项全部通过，48.032 秒，无跳过；全量 venv 不具备构建工具时的 wheel 跳过由这次独立实测覆盖。当前真实新名称闭环、最终全量及完整边界见[阶段 28–31 验证](verification-stage28-31.json)。
