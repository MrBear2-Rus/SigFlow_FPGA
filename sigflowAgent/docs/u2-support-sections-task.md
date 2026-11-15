# U2 教学辅助分区接入任务

目标是在现有课程引用、学习记录、检查器与状态分区展示正式宿主提供的受限数据。

- [x] S1 课程。公开 U2 已提交的课程检索状态和背景引用，分别标明背景材料与卡片实际引用。能力状态读取 U2 扩展，覆盖课程库可用但检索无命中、规则卡无引用和恢复后失效。
- [x] S2 学习记录。通过 UA-10 已有公共接口读取历史。正式组合层装配本地 SQLite 适配器，沿用工程目录、学习者与会话绑定。查询限量并保留来源，查询延迟返回 pending，失败返回 unavailable。
- [x] S3 检查器。提供只读运行检查投影，显示卡片证据核验和复测工具状态。工程功能正确性保持未评估。诊断命中和工具执行成功分别展示。
- [x] S4 原生分区。复用现有状态模型和后台请求，接入三个既有分区，校验运行、修订和状态版本，切换上下文时隐藏旧内容。
- [x] S5 自动化验收。正式宿主教学测试、UA-10 回归、真实综合/仿真联合验收和主程序构建通过。手工界面签收单独列为待验收项。

接口采用现有 U2 扩展的追加字段及只读路由，沿用宿主认证和学习者归属校验。课程和学习数据依赖既有提供器，教学上下文复用当前卡片模型调用。

## 正式接口

U2 能力增加 `course_available` 和 `course_reason`，使用数据库及检索就绪状态。原有 `course_configured` 表示已装配提供器。运行状态追加 `course_context`，包含状态、检索状态、配置标志及最多 8 条背景引用。原有卡片 `course_refs` 继续表示该卡实际声明的引用。

运行状态追加 `checks`，分别报告 `card_evidence`、`verification_tool`、`functional_correctness`。前者核验已绑定的证据引用，中间项保留工具 Job 状态，最后一项固定为 `not_assessed`。

`GET /api/v1/u2/runs/{run_id}/records` 返回 `u2.learning-records.v1`。字段契约见 `contracts/edu-agent/v1/schemas/u2-learning-records.schema.json`。此路由沿用 Bearer 与 X-Learner-ID 校验，并要求当前保存的工程修订有效。服务从 UA-10 公共 `MemoryHost.bind().get_learning_timeline()` 读取至多 4 页、最多 12 条记录，超出部分由 truncated 标明。SQLite 使用线程内连接。

## 配置与数据范围

正式 U2 组合默认尝试装配随仓库提供的 UA-10 simple 后端。Windows 与 POSIX 启动器追加本地 src/vendor 搜索路径，沿用当前 Python 环境。设置 `SIGFLOW_U2_MEMORY_DISABLE=1` 或显式传入 `teaching_memory=None` 可以关闭，缺少模块时保留教学服务并报告未配置。

数据保存在可信 bootstrap 工程数据目录下的 `u1-u2/teaching-memory`。查询范围是同一学习者、工程和宿主 instance 内的授权历史。UA-10 保留删除账本和去重职责，跨新 instance 的身份续接需另行定义，当前接口不会合并不同 instance。

U2 的诊断名称在本组适配层转换为 UA-10 注册概念。drivers 对应 multiple_driver，ports 对应 port_connection，syntax 对应 syntax_error，width_signedness 对应 bit_width，combinational_feedback 对应 combinational_loop，simulation_mismatch 对应 simulation_semantics，latch/reset 保留。job_observation 仅保存中性教学元数据，概念列表为空。U2 outbox 保留原始事实，UA-10 实现及课程库保持独立。

实际 SQLite 历史读取具有 1 秒上限，其他通用历史端口沿用 250 毫秒上限。该修复覆盖原先在编译负载下偶发丢失历史的回归，同时对超时读取继续降级。

学习记录初次读取等待至多 1 秒。同一教学状态的后续请求可以接收一次已完成的慢查询，交付前重新核验接收端的会话授权与事件撤销状态。尚有该会话的事实待交付时返回 `pending`，确保结束事件完成交付后才返回 `ready`。界面在限量范围内优先读取最近记录，并按时间顺序展示。已启用学习记录的原生界面可在教学结束后继续有限次数的只读刷新。

事实投递保持同一会话内的事件顺序。失败事件保留原始标识并延迟重试，其他会话仍可继续投递。UA-10 无法直接接受的 U2 学习者标识在适配层使用稳定别名，既有有效标识继续使用原有档案。

课程本地笔记保留标题、章节和引用标识，链接使用课程契约支持的 `null`。通过校验的 HTTPS 引用继续携带原链接。

## 验收范围

原生界面复用既有折叠分区，未完成手工点击签收。自动化验收覆盖正式 HTTP 路由、身份和修订隔离、数据库重启、课程背景与实际引用的区别，以及真实 Core/Yosys/Verilator 工作流。功能正确性、教师评审和跨新 instance 的学习身份续接仍独立验收。

2026-11-07 验收结果。

- 正式宿主教学测试 23 项通过，包含撤销后不复用已完成查询结果的回归。
- UA-10 回归 8 项通过，保留跨会话、预算、来源、删除与重放断言。有界延迟回归使用确定性的 300 毫秒适配器响应，实际 SQLite 联合链由原跨会话测试覆盖。
- 启动器测试 3 项通过。
- 真实 Core/Yosys/Verilator 联合验收通过。3 个 Job，课程上下文、学习记录、运行检查均记录为 verified，模型调用为 0。报告位于 `build-main-msvcrt/u2-support-sections-acceptance.json`。
- `cmake --build build-main-msvcrt --target sigflow --parallel 6` 返回 0，正式程序位于 `build-main-msvcrt/sigflow.exe`。此前运行中的旧 DLL 曾阻止复制，关闭占用后已通过完整构建。

2026-11-15 缺陷回归结果。

- 正式宿主教学接口 23 项、UA-10 联合回归 14 项、UA-10 时间查询 4 项通过。回归覆盖课程本地笔记、跨会话投递隔离、结束事件待投递、慢查询回收、撤销校验和最近记录的展示顺序。
- 原生客户端通过本机 HTTP 探针验证：教学结束状态先返回，后台只读查询随后补齐学习记录。验证不涉及 GUI 的手工点击。
- CMake 构建主程序、正式宿主网关和仿真前端测试目标成功。CTest `eda_sim_frontend_smoke` 通过。全加器的独立 RTL 回归通过 200 次结果校验，插桩点 20/20 命中；生成的 VCD 校验了 70 ns 截止与 80 ns 全量波形。
- 正式宿主联合验收通过，完成 3 个真实 Job，课程上下文、学习记录、教学时间线和运行检查均通过。报告位于 `build-main-msvcrt/u2-bugfix-acceptance.json`，模型调用数为 0。

复测入口。

```powershell
$env:PYTHONPATH = "$PWD\agent\src;$PWD\sigflowAgent;$PWD\sigflowAgent\ua10_module\src;$PWD\sigflowAgent\ua10_module\vendor"
& .\sigflowAgent\.venv\Scripts\python.exe -m unittest discover -s agent -p 'test_teaching*.py'
& .\tools\run_u2_memory_regression.cmd
& .\sigflowAgent\.venv\Scripts\python.exe -P -m sigflow_teaching_core.host.shared_acceptance --gateway build-main-msvcrt/sigflowAgent/integration/edu_u2_gateway.exe --scenario simulation --work-dir build-main-msvcrt --output build-main-msvcrt/u2-support-sections-acceptance.json --timeout 180
```
