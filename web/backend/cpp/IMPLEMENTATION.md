# C++ 后端实施记录

## 任务依据

主 contributor 的 `web/README.md` 第 3 节规定：Linux only、单一 CMake、
Node 托管 CLI 子进程、C++ 不接管队列和会话。
`web/AGENTS.md` 的“本次任务”仍是完善架构文档，README 已有该成果；
本次根据协作者的直接要求开始实施其中 C++ 范围。

## 第一批：平台基础（2026-10-31）

已迁移：
- `include/eda/api` 的进程、工具发现和所需共享类型/构建信息头。
- `src/eda-platform` 的 Linux 进程执行、路径/环境/时间、SHA256、动态库。
- `src/eda-core` 的 Logger、Version、DefaultToolchain。
- 离线 nlohmann 单头依赖（来源和校验见 third_party/README.md）。
- CMake 的 Threads/dl 目标依赖和有超时保护的基础回归测试。

构建与运行不依赖 web 外任何文件；原代码仅作为迁移来源。
`Version` 保留原构建信息 ABI 标识，不代表恢复插件动态注册。
未改 AGENTS、Node 代码，也未执行 git add/commit/push/pull/切分支。

### 迁移修正

- 去掉平台实现中的 Windows 分支。
- 工具发现遵循：configuredPath → query.bundledRoots（CLI 显式 tool-root）
  → 每工具环境变量 → 构造时默认根/SIGFLOW_TOOL_RUNTIME_DIR → PATH。
  发现结果必须是可执行的普通文件。
- 使用 exec 状态管道区分启动失败与工具自行返回 127。
- 使用单调时钟计算超时，工具退出后清理其残留进程组，防止日志管道挂起。

### 验证

Ubuntu 22.04 WSL / GCC 11.4 / CMake 3.22：

```sh
cmake -S web -B web/build -DCMAKE_BUILD_TYPE=RelWithDebInfo -DSIGFLOW_ENABLE_FTDI=OFF
cmake --build web/build -j4
ctest --test-dir web/build --output-on-failure
```

`test_platform` 覆盖 SHA256、UTF-8 路径、动态库、stdout/stderr 与流式输出、
环境变量、输出截断、非零退出、启动失败、错误工作目录、超时、取消、
残留子进程及工具发现顺序/不可执行文件。

## 待接续及需要明确的接口

此批没有改变 CLI 行为；业务命令目前仍返回未迁移的退出码 3。
后续按依赖顺序接入：
1. 单 Job Runtime、静态 provider 分发、manifest/report 和 JSONL。
2. synth/pnr/pack/flash/sim 的工具模块及实际工具链验证。
3. target/validate、structure/lint、wave、capture/devices。

接入 CLI 前需要与主 contributor 明确：
- Node SIGTERM 取消：平台层会为工具另建进程组，不能仅杀 CLI 所在组；
  CLI 需要转发取消并等待清理，SIGKILL 兜底仍须保证后代回收。
- README 给了 JSONL 的字段名，但未给所有命令的结果 payload、wave serve
  请求/响应关联、params 及 report 完整 schema；不自行冻结这些外部接口。
- 波形进程是 README 明确的常驻例外；“C++ 无跨调用状态”仅适用于一次性命令。

尚未验证 FPGA 工具、libftdi/板卡和 Node 集成，不能把基础测试通过视为整条工作流验收。

## 接口决策更新

协作者已确认沿用旧 JobRequest/JobReport 与工具参数，并补协议草案供主 contributor 审阅。
详见 `RUN_PROTOCOL_DRAFT.md`；其中标注的新增参数、路径语义和命名差异尚待集成审阅。

## 第二批：P1 本地契约基线（2026-10-31）

用户已确认 D03/D05/D07/D08：旧 snake_case 落盘、单独 project-id、Job 相对
artifact 路径、带版本的执行请求快照。Node 联调接受状态单列，未冒充完成。

新增最小 jobs.hpp（保留 JobRequest/JobRecord/JobReport/JobContext/IJobProvider，
不迁入 IJobService 队列接口和 services.hpp 容器）；JobProtocol 的状态、manifest、
report、request 及 result 序列化；contracts/run-v1 四份 schema 与五组样例。
保留旧 metrics/diagnostics 的任意 JSON 类型；无实际工具退出码时约定 -1。

Ubuntu 22.04 / GCC 11.4，沿用 task.md 的构建命令：3/3 CTest 通过：
- test_platform：原基础回归。
- test_job_protocol：实际序列化与固定旧字段 fixture 对照、请求可选值和事件转义。
- test_run_schemas：13 个样例/生成文档、schema 正反例与兼容 JSON 类型校验。

Python jsonschema 使用 WSL 已有安装，不联网、不自动安装依赖。schema 检查可选，
若其他环境缺包必须标记未执行。生产 CLI 仍为骨架，未实现 Runtime、Node 集成、
真实工具链或硬件；下一批为 P2。WSL 构建曾出现亚秒级时钟偏差警告，构建及测试
均退出 0；未将其计为业务问题或掩盖测试失败。

## 第三批：P2 单 Job Runtime（2026-11-01 完成 C++ 本地验收）

新增 JobPaths、JobRuntime、RunCommand，并接入原 CLI。实现严格参数解析、2 MiB
JSON 对象输入限制、project/jobType/jobId 目录身份验证、空目录独占认领、原子 JSON
落盘与 fsync、JSONL、SHA256、候选产物复核、累计日志截断和整个 Job 的超时预算。
不新增队列/线程池/用户会话；仅使用进程输出与取消监控线程。

用户明确确认 D09 采用任务级短生命周期 guardian。平台层先建立独立 guardian 再
释放工具启动门，通过 lifetime pipe 和 PDEATHSIG 补充父进程死亡清理；正常取消
回收工具组并落报告，随后 CLI 以原信号退出；SIGKILL 场景不假造最终报告。
EOF/EPIPE、阻塞输出、参数尚未读完时的取消均覆盖。guardian 无跨调用状态。

provider 使用 ExecutionFor/CheckedInput/ResolveExecutionTool 接收真实执行设置、
授权工程/静态资源输入和正确的工具发现顺序。request.metadata.tool_root_source
记录 argument/environment/default；显式工具根不能与环境默认 runtime 混为同一优先级。
工具发现返回绝对路径，避免切换 Job cwd 后相对可执行路径失效。

生产静态工厂目前没有真实 provider，合法但未迁入的 jobType 返回 CLI 3 和
NOT_IMPLEMENTED diagnostic，不写 Job 数据。测试 provider 仅绑定独立的测试可执行文件，
SIGFLOW_BUILD_TESTS=OFF 不生成该目标，安装规则不包含该目标。

### 最终证据

环境：Ubuntu 22.04 WSL、GCC 11.4、CMake 3.22、Python 3.10、已有 jsonschema。
沿用 task.md 配置/构建命令，最终 4/4 CTest 通过：

| 测试 | 结果 |
|---|---|
| test_job_protocol | 旧字段与本地 schema/fixture 序列化兼容 |
| test_platform | 平台层、工具发现及进程基础回归通过 |
| test_job_runtime | 51 场景通过；实际 CLI、真实工具进程、SIGTERM/SIGKILL、断管及 schema 校验 |
| test_run_schemas | 13 个样例/生成文档及协议反例通过 |

独立生产构建：

```sh
cmake -S web -B web/build-production -DSIGFLOW_ENABLE_FTDI=OFF -DSIGFLOW_BUILD_TESTS=OFF
cmake --build web/build-production -j4
```

该构建通过。进程存活测试曾遇到 /proc 读取时进程消失引发 ProcessLookupError，
已补齐该竞态处理后重新通过全部测试；没有忽略原失败。部分较早 WSL 构建出现
亚秒级时钟偏差提示，最终变更构建和全部测试退出 0。

详细调用、provider 接入、错误处理和 Node 清单见 P2_HANDOFF.md。
未修改前端/Node/Python，未改 web 外文件，未提交或推送。
真实 Node 服务、Yosys 和板卡尚未验证；P2 的本地测试不替代 P3/P4/P9 验收。

## P3：Yosys 综合（本地假工具链验证，真实工具与 Node 待验收）

迁入 YosysScriptGenerator 并去除 Windows 分支；新增 YosysProvider，接入生产静态工厂。
复用 Runtime 的项目输入、工具发现、Job 输出边界和产物摘要。非法策略/标识符、越界
或符号链接输入、非法输出路径、工具缺失、失败与无效网表不会产生成功报告。
测试使用假 Yosys；真实工具与 Node 服务因当前环境缺失尚未验证。详见 P3_HANDOFF.md。

最终验证：Ubuntu 22.04 WSL 构建、5/5 CTest 通过（其中生产 CLI 假 Yosys 综合
17 个场景）；SIGFLOW_BUILD_TESTS=OFF 生产构建通过。Windows 工作区虽有 yosys.exe，
不能作为 Linux 原生工具验收；WSL 未找到可用的 Linux Yosys 或缓存包。
Node 源码仍明确标记 HTTP/Job 调度尚未实现，因此无法进行实际 Node/SSE 联调。

补充真实工具验证（2026-11-01）：用户提供的 Windows Yosys 0.49 可从 WSL 启动，
但无法直接读取 Linux 路径。用测试专用路径转换适配器运行最小 RTL 后，CLI 退出 0、
报告 Succeeded，top.json 包含顶层且 SHA256 一致。可选复现脚本为
`tests/windows_yosys_interop.py`；不依赖外部路径进行构建，不改变生产行为。
Linux 原生工具和 Node 真实联调仍未验收。
