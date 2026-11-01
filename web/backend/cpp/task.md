# SigFlow Web C++ 后端协作任务书

版本：v0.2 · 更新：2026-11-01 · 状态：P2 C++ 本地闭环完成；跨端联调与后续模块另行验收。
工作区：`web/`；任务负责人：C++ 后端协作者；集成方：主 contributor / Node 实现方。

## 1. 目标与依据

在已有 Web 技术框架内，交付 Linux 上可独立构建、由 Node 托管的
`sigflow-tool`，完成 FPGA 工具链、仿真、分析、波形查询及硬件采集能力迁移。
优先交付一条可验证的综合链路，再逐步扩展；不能以全部目标可 configure
或 CLI 返回占位结果作为业务完成。

依据及文档分工：

| 文档 | 作用 |
|---|---|
| `../../AGENTS.md` | 仓库操作红线，不修改 |
| `../../README.md` 第 3、6、7、8 节 | 主 contributor 的架构、职责及构建约定 |
| 本文件 | 后续执行顺序、约束、契约检查点及验收清单 |
| `RUN_PROTOCOL_DRAFT.md` | run 协议评审草案，尚未成为已实现接口 |
| `IMPLEMENTATION.md` | 实际完成内容、验证环境及结果 |
| `third_party/README.md` | 离线依赖来源和校验记录 |

用户已经确认：沿用旧 `JobRequest`、`JobReport` 和各工具参数，补接口草案
供主 contributor 审阅。该确认不等于所有新增字段或路径语义已经获集成方接受。
出现真实文档冲突时列出差异，先询问再实现受影响部分；独立部分继续推进。

## 2. 不可违反的约束

- 修改、构建产物和新增文档均在 `web/` 内；外层旧代码仅用于阅读及迁移来源。
- 迁入的源码、资源和第三方依赖必须自包含。编译、安装及运行不得引用外层文件。
- 不修改 AGENTS.md；不执行 git add/commit/push/pull、切分支或其他 Git 状态变更。
  可以执行 status/diff 等只读检查。代码交由用户/主 contributor 提交与集成。
- 只支持 Linux，C++20 / C99，唯一构建文件为 `web/CMakeLists.txt`。
- 不引入 wxWidgets、C++ HTTP 服务、GUI 记账层、原插件动态注册/能力协商。
- CMake 不联网；不下载或构建 Yosys、nextpnr、Apicula、Verilator、openFPGALoader。
- 依赖按 target 声明；不使用全局 include_directories/link_libraries。
- Node 负责身份鉴别、工程管理、队列、并发、重试、会话、设备租约、Agent 托管。
  C++ 不新增这些机制的第二套权威状态。
- C++ 一次执行一个 Job；wave serve 是会话级状态进程的明确例外。
- 不重写 Python Agent；不顺带修改前端/Node。集成所需变动先以契约和联调清单提出。
- 不把本机回环网络或进程隔离视为多租户安全沙箱；OS 隔离由部署/Node 方负责。
- 硬件测试只针对明确指定且已授权的设备，仿真测试不得触发烧录或采集副作用。

## 3. 交付范围与模块边界

| 模块 | C++ 交付 | 明确由其他方负责 |
|---|---|---|
| platform/core | Linux 进程、工具发现、执行事实、报告、产物摘要 | 队列、全局状态恢复、用户鉴权 |
| synth/pnr/pack/flash | 参数校验、工具执行、日志解析、结果校验 | 提交、重试、设备租约、展示 |
| sim.build/sim.run | 仿真构建、执行、VCD 产物 | 工程编辑、仿真工作流 UI |
| target/validate | 板卡与引脚数据、约束最终校验 | 表单、即时提示和引脚配置 UI |
| structure/lint | 结构数据和语法诊断 | 编辑器、树渲染及跨面板联动 |
| wave | VCD 读取、索引、区间查询及必要的数值分析 | 绘制、选中信号、界面状态 |
| devices/capture | Linux 设备访问、TraceBridge 采集和原始结果 | 设备排队、会话向导、报告解释 |

工具模块采用静态 jobType → provider 分发。迁移旧 provider 时，保留核心算法
与参数行为，剥离 PluginHost/注册锚点/宿主上下文耦合；不能因为 README 写
“原样”而直接引入不允许的基础设施。共享旧类型中的兼容字段不代表恢复旧框架。

## 4. 契约状态与决策表

状态含义：已确认＝可以据此实现；提案＝需要集成评审后冻结；待盘点＝不能猜测。

| ID | 事项 | 状态/行动 | 阻塞范围 |
|---|---|---|---|
| D01 | Node 调度、C++ CLI、JSONL、Linux、单 CMake | 已确认，遵循 README | 无 |
| D02 | 旧 JobRequest/JobReport/工具参数 | 用户已确认沿用 | 无 |
| D03 | manifest/report 使用旧 snake_case；事件按 README camelCase | 用户已确认；已实现本地基线，Node 联调待验收 | 落盘协议冻结及 Node 联调 |
| D04 | 报告路径 reports/report.json | README 已明确；旧 job-report.json 由 Node 兼容适配 | 集成测试须覆盖路径变化 |
| D05 | --project 是路径；另增 --project-id | 用户已确认新增；CLI 接入在 P2 | run 参数冻结 |
| D06 | flash 显式确认信号，如 --confirm | 保留旧 requireConfirm 语义；传递形式待确认 | flash 接入 |
| D07 | Artifact.path 改为 Job 目录相对路径 | 用户已确认相对路径；Runtime/Node 接入待验收 | artifact 联调 |
| D08 | request.json 完整 schema/version、输入快照标识 | 用户已确认执行请求快照；schema 已落地，源码快照由 Node 提供 | 可复现执行验收 |
| D09 | CLI/工具进程取消、SIGKILL 兜底 | 用户已确认独立短生命周期 guardian；SIGTERM/SIGKILL/断管本地验证通过，Node 实际联调待验收 | 取消端到端验收 |
| D10 | wave serve 关联 ID、消息方法、错误及数值编码 | 待盘点旧 API 后出草案 | wave 对外接口 |
| D11 | structure/lint/validate/target/devices/capture 结果 payload | 待盘点旧数据后出草案 | 各命令集成 |
| D12 | 限额默认值、日志溢出行为、波形性能阈值 | P2 日志保留截断、2 MiB 参数上限已实现；部署默认限额与波形阈值待确认 | 负载/性能验收 |

此表是协作待办，不要求一次性冻结所有后期模块。执行顺序是先解决当期依赖的项。
对外契约一旦冻结，在 web 内保存 schema、样例和对应测试；变更需说明兼容性。

## 5. CLI 与数据契约

### 5.1 命令面

| 命令 | 输入概要 | 实现批次 |
|---|---|---|
| run | job-type/work-dir/job-id/project/params、timeout-sec、max-output-bytes | P2–P4 |
| target show | id、data-dir | P5 |
| validate cst / pins | cst/design 与 pins JSON | P5 |
| structure / lint | file | P6 |
| wave signals / query | vcd、signals、begin/end | P7 |
| wave serve | stdin/stdout JSONL | P7 |
| devices list | Linux 设备枚举 | P8 |
| capture run | work-dir、contract | P8 |

默认资源：编译期 data-dir，可被 SIGFLOW_DATA_DIR 和显式 --data-dir 覆盖；
显式参数优先。工具发现：显式路径/--tool-root → 工具环境变量 → runtime → PATH。
CLI 必须将这些选项落实到模块，不仅在帮助文本列出。

统一拒绝：未知命令/选项、重复且冲突的参数、缺少值、非法枚举、负数限额、
数值溢出、不完整 JSON、非对象 params、过大输入。路径或参数不得拼成 shell 命令。
`--params -` 读取单次完整 JSON；wave serve 才是逐行多请求协议。
help/version 允许输出普通文本；业务命令 stdout 只允许机器可解析 JSONL。

### 5.2 JobRequest / provider

保留旧字段：jobType、pluginId、projectId、params、metadata、requireConfirm、timeoutSec。
params 为旧工具参数对象，如 synth 的 top_module/source_files/strategy/yosys_family；
不得悄悄重命名。每个 provider 在迁入时输出参数清单：字段、类型、必填、默认值、
路径用途、允许值、副作用及失败码。以实际使用字段为准，不能只复制不完整的 paramsSchema。

pluginId 由静态映射确定；metadata 来自可信宿主而非用户 params。
C++ 只接收单任务，不提供 submit 队列、retry、setConcurrency 或跨任务 session。
provider 通过 JobContext 提供日志、进度、产物、指标及 IProcessHost；不直接打印 stdout。

### 5.3 Job 目录与写入

```text
<project>/.sigflow/agent/jobs/<jobType>/<jobId>/
  manifest.json
  request.json
  artifacts/
  logs/
  reports/report.json
```

Node 创建并授权 work-dir；C++ 不接管工程编辑/保存。作业身份和目录必须匹配；
拒绝覆盖不属于当前调用的已完成 Job。输入可来自 Node 显式授权的只读快照或静态目录；
输出、日志、缓存、VCD sidecar 和临时文件均在当前获准的 Job/工程范围。

实现路径检查时覆盖绝对路径、..、符号链接、未存在输出的父目录、嵌套目录及资源路径逃逸。
仅做字符串前缀比较不算目录约束；OS 沙箱仍不可省略。
manifest/report 原子替换；若承诺崩溃持久性，需同步文件及父目录，不能仅靠 stream.flush。
报告写入失败不能返回成功或伪造已落盘的 result。

### 5.4 报告、状态、事件

JobReport 保留旧 schema_version=eda.jobreport.v1，字段：
job_id/job_type/plugin_id/state/exit_code/artifacts/metrics/diagnostics。
Artifact 保留 id/path/schema/sha256/role；注册前检查真实文件、授权范围和摘要。
成功要求预期产物通过校验，不能仅检查进程退出 0。失败可保留已验证的诊断产物，
不得把残留/半成品当作有效综合或位流结果。

Node 持有 Queued 与总体状态；C++ 写执行事实 Running、ValidatingArtifact 和终态。
终态沿用 Succeeded/Failed/Cancelled/TimedOut。Node 合成状态时不得被晚到事件回退。

| JSONL type | 基本字段 | 规则 |
|---|---|---|
| log | level、line | 转义引号/控制字符；原始工具日志不能穿透 stdout |
| progress | percent、status | 语义阶段值，不伪装精确进度 |
| diagnostic | severity/code/message/file/line | 路径、行号、严重级别按冻结的 schema 输出 |
| artifact | id/path/role/sha256 | 与落盘 Artifact 一致 |
| result | state/exitCode/jobId | 正常可报告路径只出现一次；最终报告成功落盘后输出 |

进程退出码：0 成功，2 用法错误，3 命令迁移中，4 执行失败。
result.exitCode/report.exit_code 是执行结果码，不一定等于 CLI 退出码。
缺工具、启动失败、超时、取消、内部错误无真实工具退出码时，须在 P1 冻结明确映射，
不得伪称工具自行返回某个值。信号退出保留实际信号语义；强杀/崩溃允许无 result，
由 Node 结合退出状态及残留报告处理。EPIPE/Node 消失时应清理工具，不无限运行。

### 5.5 进程与硬件

- exec 参数数组，设置明确 cwd/environment；不执行用户提供的裸命令行。
- 区分启动失败与真实工具 exit 127；单调时钟超时；限制内存日志并持续排空管道。
- 正常取消通知单 Job Runtime，由其终止工具进程组并 wait/reap，避免僵尸和后代存活。
- signal handler 仅做安全通知，不能写 JSON、锁 mutex 或直接调用复杂 C++ 代码。
- 现有工具进程组与 Node 的 CLI 进程组不同；仅 kill(-cliPid) 不能证明后代回收。
  SIGKILL 无法被 CLI 捕获，硬兜底需 Node/OS 记录并清理工具组或使用 cgroup 等约定。
- 明确超时涵盖整个 Job 还是单次工具；建议 Runtime 使用 Job 级剩余预算，避免多工具累计超时。
- 日志保留上限不等于工具输出终止阈值；按 D12 冻结截断/终止策略及报告标记。
- flash/capture 先确认租约和目标设备；C++ 校验设备身份和输入，Node 负责跨任务互斥。

## 6. 分阶段任务与验收

勾选表示已实际验证；“实现/测试通过”与“主 contributor 集成接受”分开记录。
每阶段完成后更新 IMPLEMENTATION.md：文件、契约版本、验证命令、结果、未覆盖项。

### P0：基础层迁移（已有成果）

- [x] Linux 平台实现、进程执行、SHA256、路径、动态库迁入。
- [x] Logger、Version、DefaultToolchain 迁入。
- [x] nlohmann 单头离线依赖及来源/hash 记录。
- [x] Threads/dl 的 target 依赖，Ubuntu WSL 构建及基础测试通过。
- [x] 工具发现顺序、可执行权限、启动失败、超时/取消、残留管道等基础回归。
- [ ] 作为后续运行库接入 CLI，并通过集成方审阅；基础测试不代替这一验收。

### P1：冻结第一条 run 链路的契约

依赖：P0。产物：web 内契约头、JSON schema、成功/失败样例、兼容性对照。

- [x] 盘点 jobs.hpp/JobContext/provider 及传递依赖，迁入最小自包含 API。
- [x] 提取旧序列化，列出 manifest/report/request 的实际字段与 schema。
- [x] 解决 D03/D05/D07/D08 和结果码映射；flash 可暂留到 D06 决策后实施。
- [x] 记录旧字段、路径和报告文件名与新框架的差异，供 Node 适配。
- [x] schema 验证成功、失败、空产物、诊断及旧参数 fixture，不以文档示例充当全部类型。

- [ ] Node 独立读取 fixtures 并完成跨端契约验收（尚未实现，不因 C++ 本地通过而勾选）。

通过条件：Node 与 C++ 能据同一组 fixture 独立读写，不需要解析自由文本或猜测字段。

### P2：单任务 Runtime 与 CLI 纵向闭环（C++ 本地完成）

依赖：P1。建议落点：src/eda-core/JobRuntime.*、src/cli/ 的解析/事件/分发模块。

- [x] 实现严格参数解析、JSON 装载及 work-dir/输入输出授权检查。
- [x] 实现 JobContext、静态 provider 分发、manifest/request/report、摘要及 JSONL。
- [x] Runtime 不引入队列、线程池、全局项目索引或重启恢复。
- [x] 测试专用 provider 只进入测试目标，不暴露生产通用命令执行入口。
- [x] 实现用户确认的 D09 正常取消与 guardian 强杀兜底，覆盖退出时序和 EPIPE；调用约定见 P2_HANDOFF.md。
- [ ] 主 contributor 完成实际 Node 服务接入与跨端验收（C++ 本地模拟 detached 调用已通过，不能代替此项）。
- [x] 测试成功、参数错误、未知/未迁移 job、启动失败、非零退出、写盘失败、取消、超时。
- [x] 测试中文/空格路径、JSON 特殊字符、输出限额、路径逃逸及重复 Job 目录。

验证证据：2026-11-01，4/4 CTest 通过，其中 Runtime 51 个真实 CLI 场景；测试关闭的生产构建通过。
详细契约、限制和 Node 交接见 P2_HANDOFF.md，实施记录见 IMPLEMENTATION.md。

通过条件：无需 FPGA 工具即可从 CLI 参数走到可解析事件、正确报告与受限产物；
失败路径没有成功 result，取消后没有遗留工具进程。

### P3：综合 synth（第一条真实工具链）

依赖：P2。参考旧 eda-synth-yosys；新落点 src/plugins/synth-yosys。

- [x] 迁入脚本生成、日志和产物校验，移除旧注册框架依赖。
- [x] 参数清单覆盖 schema 和 startJob 实际读取字段，限制 output_json 等输出路径。
- [x] 显式配置/工具根/环境/runtime/PATH 全链路接到实际 provider。
- [x] 假工具 fixture 覆盖缺工具、非法参数、无产物、损坏产物、非零退出及长日志。
- [ ] 真实 Yosys 运行最小 RTL，验证 top.json 可读取、摘要和报告一致：Windows 版通过测试专用 WSL 路径适配器已通过；Linux 原生环境仍待验证。
- [ ] 与 Node 对接一次提交、SSE 日志、成功/失败展示及产物下载。

通过条件：真实综合闭环通过；没有真实工具时只能标记 fixture 验证通过，不能勾选真实验收。

### P4：pnr、pack、仿真和 flash

依赖：P3；flash 额外依赖 D06/D09 和 Node 设备租约。

- [ ] 分别迁入 pnr-nextpnr、pack-gowin、sim-verilator 的构建与运行 provider。
- [ ] 每个 provider 单列参数/工具版本/依赖产物及输出校验，不共用一份模糊验收。
- [ ] 验证 synth → pnr → pack；任务由 Node 串联，C++ 不添加后台流水线调度器。
- [ ] 验证 sim.build → sim.run → VCD；构建缓存属于明确授权工程范围。
- [ ] 迁入 program-openfpgaloader；无确认、无租约、设备不匹配和位流无效时拒绝执行。
- [ ] 验证取消、超时、工具缺失/版本不兼容和跨步骤产物不匹配。
- [ ] 真工具链测试及授权板卡烧录分别记录；不得用 mock 代替硬件验收。

### P5：目标板及约束

依赖：P1；与 P3 可在不冲突时独立推进。

- [ ] 迁入 target-profiles 静态数据、target 与 cst 模块；静态数据只读。
- [ ] 完成 target show、validate cst、validate pins 的 D11 schema/fixture。
- [ ] 覆盖器件/封装不匹配、未知/重复引脚、非法 IO、缺文件、格式错误。
- [ ] Node 与 C++ 使用同版本板卡数据，避免 UI 允许而执行端含糊拒绝。

### P6：structure 与 lint

依赖：P1、离线 tree-sitter 运行库及 Verilog grammar。

- [ ] vendoring、许可证、版本、hash；大 parser 独立编译目标，保持离线构建。
- [ ] 迁入旧 TreeSitterLinter/VerilogStructuring，去 wxString 和 UI 依赖。
- [ ] 冻结结构节点、稳定标识、源码定位、诊断严重级别和范围语义。
- [ ] 覆盖合法/非法 RTL、空文件、层次结构、UTF-8 与 CRLF 的行列定位。
- [ ] 与 Node 编辑器 fixture 核对，不能依赖 GUI 的选中或缓存状态。

### P7：波形 worker 与分析

依赖：P1、离线 VCD 依赖、D10/D12。

- [ ] 迁入 trace/wave-vcd 必要逻辑，去 wx，索引/sidecar 不写静态资源目录。
- [ ] 定义 open/close/signals/query 的关联 ID、错误、EOF/损坏 JSON、文件切换协议。
- [ ] 使用显式文档化编码表达超出 JS 精确整数范围的时间值，保留时间单位和精度。
- [ ] 定义边界值、X/Z、别名、多位值、重复时间戳、空信号及区间分页语义。
- [ ] 覆盖请求/响应大小、打开文件上限、超长单行、缓存释放及 worker 被重建。
- [ ] 对齐/对比/解码按旧能力单独列样例，不把 UI 操作状态迁到 C++。
- [ ] 选定数据集、规模及首屏/区间查询/内存阈值，与 Node 方确认后验收。

### P8：设备与 TraceBridge

依赖：P4/P5、Node 设备租约、D11。

- [ ] 迁入 Linux SerialPort/Enumerator、TraceBridge transport/capture/必要调试逻辑。
- [ ] wxString/std::filesystem 改造，复制所需 rtl/debug 资源并记录来源。
- [ ] libftdi 可选；无库、无设备、权限不足、断线必须明确报告，不能假成功。
- [ ] 设备稳定标识、确认/租约信息、采样配置和输出结构形成 fixture。
- [ ] fake transport 验证超时、取消、断线、坏帧和部分结果。
- [ ] 授权硬件验证烧录/采集互斥、采集数据/VCD、报告和恢复行为。

### P9：独立部署与最终交接

依赖：拟交付模块的对应阶段，未迁入能力必须明确保留未实现状态。

- [ ] 全新 Linux 环境只使用 web 配置/构建/运行，离线依赖齐备。
- [ ] 测试开/关、libftdi 有/无、缺少外部工具均给出预期行为。
- [ ] cmake --install 后从安装目录启动，验证资源路径覆盖和部署前缀可迁移。
- [ ] 部署后不依赖源码目录的编译期默认路径；外部工具路径/动态库部署责任写清。
- [ ] Node 联调覆盖成功、失败、断开、取消、超时、SIGKILL 和重启后的残留事实。
- [ ] 所有新增契约/样例/依赖来源/验收结果位于 web，形成主 contributor 可审阅交接。
- [ ] git diff --check；确认没有 web 外改动、构建产物或密钥混入交付文件。

## 7. 验证命令与证据规则

从仓库根目录执行；生成文件仍在 web 内：

```sh
cmake -S web -B web/build -DCMAKE_BUILD_TYPE=RelWithDebInfo -DSIGFLOW_ENABLE_FTDI=OFF
cmake --build web/build -j4
ctest --test-dir web/build --output-on-failure
cmake --install web/build --prefix "$PWD/web/build/install"
```

测试分类：基础单测、协议 fixture、假工具/transport 集成、真实 EDA 工具、Node 联调、
真实硬件。分别报告结果，缺少依赖标记“未验证/跳过及原因”，不能合并为全通过。
状态不只写“完成”：必须记录版本、执行命令、代表产物与失败路径验证。
安装测试、硬件测试等只在对应阶段运行；不为纯文档改动重复跑全套工具链。

## 8. 执行与协作流程

1. 每次工作先读本文件当前阶段与未决决策，检查工作区现状，保护他人改动。
2. 一次交付一个可评审增量：相关代码、冻结契约、必要测试和实施记录一起更新。
3. 外层旧代码先检查依赖再迁移，复制后保证 web 自包含，不用绝对路径临时“编通”。
4. 当前阶段涉及未决外部语义时，给出具体兼容差异和提案，询问用户/集成方；
   不擅自把提案标记已确认，不因此停止其他已明确工作。
5. 发现必须改 Node/部署配置时，以交接清单说明字段、调用、预期结果和失败行为；
   不擅自替主 contributor 重构该层。
6. 阶段结束更新本文件复选框及 IMPLEMENTATION.md；契约草案与最终 schema 保持一致。
7. 默认不提交、不推送；用户明确授权覆盖仓库规则后才执行相应 Git 操作。

## 9. 当前下一步

P0/P1/P2 的 C++ 本地实现已通过 Ubuntu WSL 验证，Node 实际服务联调仍待集成方验收。
P3 已迁入 Yosys provider 并通过假工具链验证；真实 Yosys 和 Node 联调仍待可用环境，
不能将假工具通过视为 P3 全部验收通过。其余模块按依赖推进。
