# SigFlow虚拟化云平台设计方案 v0.2
## 整体架构
本项目将原来基于cpp,wxWidgets的SigFlow项目迁移到linux服务器上提供网页服务。原项目在此目录外部(../web)
本项目由`html+css+js`负责UI前端交互,js作为后端主进程，管理各个组件，代替wxWidgets的工作。
尽可能缩小c++的能力范围，将其限制在c++专有的工具链上，如RTL,nextpnr,yosys等。
## 文件组织
项目根目录下的文件夹./web/为此方案的工作区，云平台所有代码将迁移至这个文件夹下，
./web/外的所有文件仅供改动时比对参考，实际部署时以./web/为根目录，执行本计划期间，不得修改，不得依赖web外的任何文件。
```文件架构
.
├── AGENTS.md
├── backend 后端
│   ├── userprojects : 用户项目存在此文件夹下，每个用户在其下存储一个子文件夹(名为用户id),他的文件隔离在其下
│   ├── database  : 数据库，保存用户数据，id,名称，密码等
│   ├── cpp  : 所有cpp代码
│   └── nodejs  所有js代码
├── CMakeLists.txt
├── frontend  前端
│   ├── pages  :html页面
│   ├── scripts  : 前端js代码
│   └── styles   : css代码
│   ├── assets   :资源文件，如图片等
└── README.md  
```
## 功能划分
以下按两类划分：JS 独立负责网页应用、工程数据和用户工作流；C++ 提供必须访问本机工具链、操作系统或硬件的能力，由 JS 按需调用。当前桌面程序中的 wxWidgets UI 改为网页实现；这里列出的是目标网页平台的职责划分，不表示这些能力已经全部迁移完成。
### JS独立负责的模块
- **网页应用界面**：主工作区、导航、菜单、工具栏、标签页、面板布局、主题、弹窗、快捷键、状态栏及用户偏好。
- **项目管理与文件读写**：新建/打开/关闭项目、最近项目、项目配置、文件树、文件增删改、保存、自动保存、未保存提示及工程状态。项目在云端时由 JS 调用云端服务；访问本机文件时使用浏览器授权能力或本机文件接口。
- **RTL编辑器**：RTL 文本编辑、语法高亮、搜索、跳转、折叠、差异查看、编辑状态和诊断结果展示；编辑器不负责启动综合或仿真工具。
- **结构树与属性面板**：模块、信号、实例等设计结构的浏览和编辑，属性表单，以及代码、结构树和画布之间的选中、定位和联动。
- **画布编辑**：元件库浏览、元件放置、连线、文字、选择/多选/框选、拖动、删除、缩放、平移、属性编辑、布局保存与恢复；画布数据和交互状态由 JS 管理。
- **编辑操作**：撤销/重做、复制/粘贴、删除、重复、编辑命令历史和快捷操作。现有桌面端有些菜单操作尚为占位，网页实现时按目标功能实现，不把占位行为当作已完成能力。
- **目标板与约束配置界面**：目标板/器件选择、引脚绑定、IO 属性、约束编辑、导入/导出、预览、冲突提示和参数表单。JS 负责配置交互与即时提示；工具执行前的最终校验由 C++ 完成。
- **FPGA工作流界面**：综合、布局布线、打包、烧录的参数设置、任务提交、任务列表、进度/日志/报告展示、取消、重试和结果导航。JS 管理面向用户的流程和展示状态。
- **仿真工作流界面**：仿真配置、顶层选择、编译/运行操作、任务状态、日志、错误、缓存操作入口和仿真结果导航。仿真程序的实际构建和运行由 C++ 工具桥接执行。
- **波形查看与交互**：VCD 波形绘制、信号树、信号搜索、添加/移除信号、显示格式、分组、别名/注释、时间轴、缩放、标记、测量、模式搜索、事件列表及源码/设计导航。波形界面由 JS 实现；数据读取可调用 C++ 查询接口。
- **波形分析与对比界面**：仿真波形与硬件采集波形的选择、时间对齐、双轨对比、事件定位、UART 解码结果和协议报告展示；用户操作和视图状态由 JS 管理。
- **TraceBridge配置与工作流界面**：调试契约、探针、触发条件、采样深度等配置；采集向导、设备选择、运行状态、暂停/取消、会话历史、报告和恢复提示。JS 负责配置界面及流程状态。
- **Agent与教学交互**：Agent 会话、聊天、提示、计划卡、审批交互、运行状态和报告解释。JS 负责网页侧主进程/调用协调；现有 Python Agent 保持独立运行，由 JS 通过其服务接口调用，不要求为了替换 wxWidgets 而重写 Python Agent。
- **插件面板与工具状态展示**：网页中的插件入口、插件面板、能力/状态展示和用户交互。现有 wxWidgets 插件界面不能直接作为网页 UI 使用；网页插件界面需由 JS 实现。
- **项目级展示与导出交互**：工程报告、日志、综合/布局布线资源与时序结果、产物列表、打开/下载/导出等网页交互。若导出依赖本机工具或本机文件，由 JS 发起请求并展示结果。
### C++独立负责的模块
- **FPGA工具链执行**：发现并调用 Yosys 综合、nextpnr 布局布线、Apicula/gowin_pack 打包、Verilator 仿真以及 openFPGALoader 烧录等本机工具；管理进程、工作目录、环境、取消、超时、日志、退出状态和产物校验。
- **仿真执行与波形数据服务**：调用 Verilator 完成仿真构建和运行、生成 VCD 等结果；按需提供波形文件读取、信号/跳变查询或分页数据。波形绘制、信号管理和用户交互仍由 JS 负责。
- **硬件设备访问**：本机设备枚举与连接、串口/FTDI/JTAG 通信、FPGA 烧录及硬件数据采集；处理设备占用、通信错误和连接生命周期。烧录等有副作用的操作须由本机侧执行安全检查。
- **TraceBridge本机执行部分**：生成或准备调试所需输入，调用工具构建调试位流，执行硬件采集、协议通信和原始数据读取，并返回采集结果；调试配置表单、会话管理界面和波形对比界面由 JS 负责。
- **工具输入与产物处理**：将 JS 提交的工程快照/配置转换为工具输入，检查必要文件和工具输出，生成或登记报告、日志及产物标识；不接管 JS 的工程编辑状态。
- **本机文件和进程接口**：仅在工具执行确实需要访问用户本机文件时，访问经授权的工程目录并管理临时文件；不负责云端工程状态、网页文件树或普通编辑保存。
- **RTL硬件描述**：项目中的 Verilog/SystemVerilog 文件及 FPGA 内运行的 TraceBridge 逻辑继续作为 RTL，由 FPGA 工具链读取、综合和实现。RTL 本身不是 C++ 代码，也不由 JS 重写；C++ 负责工具调用和必要的输入/输出桥接。
- **现有C++工具/插件后端**：保留与 EDA 工具、硬件和本机文件直接相关的插件能力，通过稳定接口向 JS 暴露能力查询、任务提交、状态/日志查询、取消和产物读取；wxWidgets 面板和页面状态不迁入该后端。
## 具体架构

本节是 `./web/` 的权威结构说明，取代上面「文件组织」中的粗略树。所有内容只使用 `./web/` 内的文件，不依赖外部路径。

### 1. 运行形态

服务器上**只有一个常驻进程**：Node 主进程。C++ 侧全部是"用到才起、跑完就退"的子进程；Python Agent 每个用户会话一个，由 Node 托管。因此 C++ 没有队列、没有会话、没有跨调用状态，并发由操作系统进程隔离保证。

```
浏览器
  │  HTTP + SSE
  ▼
Node 主进程 (backend/nodejs)  ← 唯一常驻；启动时读 config.txt
  │
  ├─ spawn 一次性  sigflow-tool run|structure|lint|validate|capture|devices
  │                每任务一个进程，stdout 输出 JSONL 事件
  ├─ spawn 每用户  sigflow-tool wave serve      （stdin/stdout JSONL）
  ├─ spawn 每用户  python -m sigflow_edu_agent  （bootstrap 走 stdin，不改动）
  ├─ 扮演 Gateway  向 Python Agent 提供 /api/v1/health、/projects/{id}/context、
  │                /jobs/{id}/report 等兼容接口
  └─ 读写          backend/userprojects/<uid>/<project>/.sigflow/agent/jobs/…
```

| 进程 | 生命周期 | 数量 | 主要职责 |
|---|---|---|---|
| Node 主进程 | 常驻 | 1 | HTTP/SSE、会话、工程管理、Job 队列与并发、设备租约、Agent 托管、静态文件 |
| `sigflow-tool <cmd>` | 一次性 | 每任务一个 | 综合、布局布线、打包、烧录、仿真、结构树/诊断、约束校验、设备枚举 |
| `sigflow-tool wave serve` | 随用户会话 | 每用户 1 | 波形读取与查询；用户退出或项目切换时重建 |
| `python -m sigflow_edu_agent` | 随用户会话 | 每用户 1 | 教学 Agent（与改造前完全一致） |

**状态归属**：Job 队列、Job 状态机、并发上限、设备租约、会话、工程索引、日志聚合全部在 Node；C++ 只往 Job 目录写执行事实（manifest/report/artifacts）。Python Agent 不直接调 C++，只与 Node 通信。

### 2. 目录结构与职责

```
web/
├── AGENTS.md
├── README.md
├── config.txt                    运行时默认值（Node 启动时读一次）
├── CMakeLists.txt                唯一的构建文件（Linux only，不联网）
├── frontend/                     纯静态前端，由 Node 托管
│   ├── pages/                    HTML 页面
│   ├── scripts/                  前端 JS（编辑器、画布、波形绘制、面板）
│   ├── styles/                   CSS / 主题
│   └── assets/                   UI 资源：图标（原 main/res）、canvas_elements.json
└── backend/
    ├── static/                   编译前即确定、Node 与 C++ 共读的资源（不放运行时产物）
    │   ├── target-profiles/      tang-nano-9k.json、tang-nano-9k.pins.json
    │   └── rtl/debug/            TraceBridge 调试 RTL（sf_micro_ila.sv 等 12 个文件）
    ├── database/                 用户表、密码摘要等持久数据（运行期生成）
    ├── userprojects/             每用户一个子目录；其下每工程一个目录（运行期生成）
    ├── nodejs/                   Node 主进程
    │   ├── package.json
    │   ├── src/
    │   │   ├── index.js          入口：读配置、装配模块、启动服务
    │   │   ├── http/             HTTP/SSE、静态文件、会话
    │   │   ├── jobs/             队列、并发、Job 目录、子进程与 JSONL
    │   │   ├── devices/          设备租约（flash/capture 互斥）
    │   │   ├── wave/             每用户波形 worker 的生命周期与协议
    │   │   ├── agent/            Python 托管 + Gateway 兼容层
    │   │   ├── projects/         工程目录、文件树、快照
    │   │   ├── plugins/          网页插件注册与能力展示
    │   │   └── util/             config 解析、日志、JSONL 工具
    │   └── test/
    └── cpp/                      C++ 工具进程（详见第 3 节）
        ├── include/eda/api/      冻结契约：Job/Artifact/Error/Toolchain/Waveform
        ├── third_party/          jsoncpp、nlohmann、vcd、tree-sitter
        ├── src/
        │   ├── eda-platform/     进程托管、动态库、sha256
        │   ├── eda-core/         日志、工具发现、单任务执行器、报告落盘
        │   ├── plugins/          8 个工具实现模块
        │   ├── analysis/         结构树 / 语法诊断（tree-sitter）
        │   ├── debug/            TraceBridge 采集/传输/分析（去 wx）
        │   ├── trace/            波形查询与 sidecar 索引
        │   └── cli/              sigflow-tool 入口与子命令
        ├── contracts/run-v1/     冻结的 run 协议（schema + fixtures）
        └── tests/                C++ 测试与 Python 集成脚本
        └── tests/
```

### 3. C++ 侧

#### 3.1 CMake 目标图

| 目标 | 类型 | 内容 |
|---|---|---|
| `sigflow_api` | INTERFACE | `include/` 冻结契约头 |
| `sigflow_jsoncpp` / `sigflow_nlohmann` / `sigflow_vcd` / `sigflow_tree_sitter` / `sigflow_tree_sitter_verilog` | STATIC / INTERFACE | `third_party/` 源码直编，不联网 |
| `sigflow_platform` | STATIC | 原 `core/src/eda-platform`（仅 POSIX） |
| `sigflow_core` | STATIC | 日志、工具发现、**单任务执行器**、manifest/report 落盘 |
| `sigflow_plugin_synth` / `_pnr` / `_pack` / `_flash` / `_sim` / `_target` / `_cst` / `_wave` | STATIC | 8 个工具模块 |
| `sigflow_analysis` | STATIC | 结构树 / 语法诊断（tree-sitter） |
| `sigflow_debug` | STATIC | TraceBridge 采集与传输（libftdi 可选） |
| `sigflow_trace` | STATIC | 波形查询、索引、缓存 |
| `sigflow-tool` | EXECUTABLE | 唯一 C++ 产物，输出到 `<build>/bin/` |

全工程只有 `web/CMakeLists.txt` 一个构建文件，所有目标都在其中用 `sigflow_add_module(<name> DIR <目录> DEPS ...)` 声明；源文件按目录 GLOB，**不出现全局 `include_directories()` / `link_libraries()`**，依赖只写在目标上。插件注册改为 `jobType → 实现` 的静态分发表，不再有 `PluginHost`、能力协商和 `eda_plugin_force_link_*` 锚点。

#### 3.2 迁移对照（原路径 → 新路径）

| 原位置 | 新位置 | 处理 |
|---|---|---|
| `include/eda/api/*` | `backend/cpp/include/eda/api/` | 只保留 Job/Artifact/Error/Toolchain/Waveform/Process；gateway 专用 schema、grant/receipt 类型不迁 |
| `core/src/eda-platform/*` | `src/eda-platform/` | 去 `win32/` 分支 |
| `core/src/eda-core/{Logger,Version,Toolchain}.cpp` | `src/eda-core/` | 原样 |
| `core/src/eda-core/JobService.cpp` | `src/eda-core/JobRuntime.cpp` | **只保留单 Job 执行 + 落盘**；删队列、线程池、`setConcurrency`、重启恢复 |
| `core/src/eda-core/{PluginHost,PluginRegistry,EventBus,CommandBus,SchemaRegistry,CoreSchemas,Project,JobsMigration,LegacyJobHistory,DeclarativeTool}.cpp` | — | 不迁 |
| `core/src/eda-agent-gateway/*` | — | 不迁（33 个文件；其中仅 2 个耦合 cpp-httplib，其余随 Gateway 一并由 Node 重写） |
| `InnerPlugin/eda-{synth-yosys,pnr-nextpnr,pack-gowin,program-openfpgaloader,sim-verilator,target-tangnano9k,cst-gowin-cst,wave-vcd}` | `src/plugins/<同名>` | 原样（含各自 `include/`） |
| `InnerPlugin/eda-lib-basic` | — | 不迁；元件库 JSON 改由前端读 `assets/canvas_elements.json` |
| `main/debug/*`（42 个 wx-free 文件） | `src/debug/` | 去 wx 改造（见 3.4） |
| `main/debug/{TraceBridgeWindow,DebugContractConfigWindow}.{h,cpp}` | — | wx UI，不迁 |
| `main/trace/*` | `src/trace/` | 去 wx 改造（`TraceSource.h` 的 `wxString`） |
| `main/{TreeSitterLinter,VerilogStructuring}.{h,cpp}` | `src/analysis/` | 去 wx（29+1 处 `wxString`），供 `structure`/`lint` 子命令使用 |
| `main/platform/{SerialPort,SerialEnumerator}.{h,cpp}` | `src/eda-platform/` | 去 wx 后并入 |
| `main/platform/{PlatformPaths.h,Log.h}` | `src/eda-platform/paths.h`、`log.h` | 重写为 std 版本 |
| `main/platform/{SimToolchain,LocalPipe,DynamicLibrary}.*`、`main/jobs/*`、`main/Simulation/*`、`main/fpga/*` | — | 均为 wx 记账层/薄壳，不迁 |
| `rtl/debug/*.sv` | `backend/static/rtl/debug/` | 资源文件 |
| `main/fpga/target-profiles/*.json` | `backend/static/target-profiles/` | 资源文件 |
| `main/canvas_elements.json`、`main/res/*` | `frontend/assets/` | 纯前端资源 |
| `3rd/{json,nlohmann,vcd,tree-sitter,tree-sitter-verilog}` | `third_party/` | 原样；tree-sitter 语法单独成库 |
| `3rd/{wxWidgets-3.2.9,httplib}`、`plugin_sdk/`、`Plugin_DeepSeek/`、`agent/`、`sigflowAgent/`、`tests/` | — | 不进 `web/` |

#### 3.3 不迁移清单（明确边界）

- 全部 wxWidgets UI：`MainFrame`、`MainMenuBar`、`main/ui/*`、所有 `*Panel`/`*Window`/`*Notebook`、`Canvas*`、`WaveformGL*`、`*TextLayer`、`*MiniMap`；
- wx 时代插件 ABI：`plugin_sdk/ISigPlugin.h`（含 `#include <wx/wx.h>`）、`Plugin_DeepSeek`、`main/PluginManager.*`；
- GUI 记账层：`main/jobs/*`（732 行 JobService + 497 行 ToolJobs）、`main/Simulation/*`、`main/fpga/*` 的 wx 薄壳；
- HTTP 服务：`core/src/eda-agent-gateway/*`、`3rd/httplib`；
- 旧构建与分发：wx 获取逻辑、`FpgaTools.cmake`（不负责编译 yosys/nextpnr）、`StageRuntimeLibs.cmake`（ldd 落位改由部署脚本处理）。

#### 3.4 去 wx 改造清单

`main/debug` 与 `main/trace` 的 49 个 cpp 文件（debug 42 + trace 7）不是"原样复制"就能编过，必须做两类机械改造：

| 改造项 | 涉及 | 说明 |
|---|---|---|
| `wxString` → `std::string` | `CaptureRunService`、`MonitorService`、`DebugFingerprint`、`MonitorIncidentService`、`InstrumentationImpact.h`、`TraceSource.h` 等 | 统一 UTF-8；跨进程只传 std 类型 |
| `../platform/PlatformPaths.h` → `src/eda-platform/paths.h` | `DebugAcquisition`、`DebugOverlayBuilder`、`DebugSession` | 去掉 `wxFileName`/`wxStandardPaths`，用 `std::filesystem` |
| 删除 `#include <wx/...>` | 上述文件的头文件 | 保持公共头不泄漏 wx |

`main/TreeSitterLinter.*` 与 `main/VerilogStructuring.*` 同样只需把 29 处 + 1 处 `wxString` 换掉，即可作为结构树/诊断的 C++ 实现（见 3.5）。

#### 3.5 CLI 契约

**权威来源**：`backend/cpp/contracts/run-v1/`（request / manifest / report / event 四份 JSON Schema 与 fixtures）。
本节只是摘要，冲突时以 contracts 与 `P2_HANDOFF.md` 为准。

| 子命令 | 用途 | 关键参数 |
|---|---|---|
| `run` | 执行一个 Job | `--job-type <synth\|pnr\|pack\|flash\|sim.build\|sim.run>`、`--project <dir>`、`--project-id <id>`、`--job-id <id>`、`--work-dir <dir>`、`--params <json-file\|->`、`--timeout-sec`、`--max-output-bytes`、`--data-dir`、`--tool-root` |
| `structure` | 结构树（tree-sitter，一次性） | `--file <path>` |
| `lint` | 语法诊断（一次性） | `--file <path>` |
| `validate cst` | 约束文件校验 | `--cst <file>`、`--pins <json>` |
| `validate pins` | 引脚绑定校验 | `--design <json>`、`--pins <json>` |
| `target show` | 目标板信息 | `--id <profile-id>`、`--data-dir` |
| `wave signals` / `wave query` | 波形信号表 / 区间跳变 | `--vcd <file>`、`--signals`、`--begin`、`--end` |
| `wave serve` | 每用户常驻查询进程 | 读 stdin JSONL，写 stdout JSONL |
| `capture run` | TraceBridge 采集 | `--work-dir`、`--contract <json>` |
| `devices list` | 本机设备枚举 | — |

**run 的调用规则**

| 规则 | 说明 |
|---|---|
| Job 目录 | Node **预先创建为空目录**，路径必须为 `<project>/.sigflow/agent/jobs/<jobType>/<jobId>`；Runtime 用 `.runtime-claim` 原子认领，目录不可复用，重试必须分配新 `jobId` |
| 参数 | `--params -` 从 stdin 读**一个完整 JSON 对象**后关闭 stdin（不是 JSONL 多请求）；文件同理；上限 2 MiB |
| ID | 1–128 位 `[A-Za-z0-9._-]`，不允许单独的 `.` 或 `..` |
| 路径 | 拒绝 `..` 段与符号链接；工作目录必须落在工程内 |
| 输入文件 | provider 只接受工程内文件，或显式 `--data-dir` 下的绝对文件 |
| 产物 | `Artifact.path` 一律相对 Job 目录且位于 `artifacts/` 下；SHA-256 登记时计算、终态前复核 |
| 确认门 | `require_confirm` 首版默认 false；flash 的确认传递方式尚未确定（见 3.6） |

**Job 目录布局**（Node 建空目录，Runtime 写）：

```
<project>/.sigflow/agent/jobs/<jobType>/<jobId>/
├── request.json        执行请求快照（sigflow.execution-request.v1）
├── manifest.json       状态机记录（eda.job.v1），随状态原子更新
├── .runtime-claim/     认领标记，保留不删；重复调用被拒绝
├── artifacts/          工具产物
├── logs/tool.log       原始工具输出（受 --max-output-bytes 截断）
└── reports/report.json 报告（eda.jobreport.v1：state/artifacts/metrics/diagnostics）
```

**stdout JSONL 事件**：字段以 `contracts/run-v1/event.schema.json` 为准。`log.line` 是有限片段、可能含换行，不能按“一条工具物理行”处理；`artifact` 事件是**候选**，只有终态 `result` 且报告校验通过才算产物成立。

**退出码与状态**

| 层 | 取值 |
|---|---|
| CLI | `0` 成功、`2` 用法/参数错误、`3` 子命令或 provider 未实现、`4` 执行失败；被信号取消时以原信号退出 |
| `report.exit_code` | 真实工具退出码；没有真实工具退出状态时为 `-1`（不冒充工具返回值） |
| `result.state` | `Succeeded` / `Failed` / `Cancelled` / `TimedOut`；只有终态才发 `result` |

**取消**：Node 必须 `detached` spawn，取消时 `kill(-pid, SIGTERM)`，留出清理与落盘时间后再 `SIGKILL`；Runtime 内有 guardian 负责回收工具进程组。Node 重启时扫描残留 `Running` 目录做恢复。

**工具发现顺序**：参数内 `yosys_path` 等 → `--tool-root` → 每工具环境变量（`SIGFLOW_YOSYS` 等）→ 默认 runtime（`SIGFLOW_TOOL_RUNTIME_DIR`）→ `PATH`；结果必须是可执行的普通文件。CMake 不下载、不编译这些工具。

**当前实现进度**：`run --job-type synth` 可用；`pnr/pack/flash/sim.build/sim.run` 返回 `3`；`structure/lint/validate/capture/devices/wave` 仍是规划。

#### 3.6 契约与协作文档索引

| 文档 | 作用 |
|---|---|
| `backend/cpp/contracts/run-v1/` | **权威**：request / manifest / report / event schema 与 fixtures |
| `backend/cpp/task.md` | 协作任务书：P0–P9 阶段划分与验收口径 |
| `backend/cpp/IMPLEMENTATION.md` | 已完成批次与迁移修正记录 |
| `backend/cpp/P2_HANDOFF.md` | 单任务 Runtime 与 CLI 的对接细节（Node 必读） |
| `backend/cpp/P3_HANDOFF.md` | Yosys 综合 provider 的实现与验证边界 |
| `backend/cpp/RUN_PROTOCOL_DRAFT.md` | 协议草案；与 contracts 冲突时以 contracts 为准 |
| `backend/cpp/third_party/README.md` | 离线依赖来源与校验和 |

**已定接口（2026-11-01）**

| 决策 | 结论 |
|---|---|
| `wave serve` 协议 | stdin/stdout JSONL，一行一条；请求带 `id`，响应回同一个 `id` |
| 其他子命令 payload | 先冻结公共信封（`id`、`ok`/`error{code,message}`），结果体随实现逐个冻结 |
| flash 确认门 | 沿用旧语义：Node 弹窗确认后下发 `require_confirm=true`，C++ 仍做最后检查 |
| 代码格式 | 统一使用 `web/.clang-format`（LLVM 基线：4 空格、100 列、花括号同侧） |
| 推进顺序 | 先做 Node ↔ C++ 的 synth 端到端联调，再扩展 P4 provider |

`wave serve` 的消息类型：`open` / `close` / `signals` / `query`，响应为 `result` 或 `error`；错误统一为
`{"id":…,"ok":false,"error":{"code":…,"message":…}}`。具体字段在 P7 实现时补进 `contracts/wave-v1/`，
在此之前不视为已冻结。

**仍需在实现时冻结**：`structure`/`lint`/`validate`/`capture`/`devices` 的结果字段；`wave serve` 的字段清单。

### 4. Node 侧

#### 4.1 模块

| 目录 | 职责 |
|---|---|
| `src/http/` | HTTP 服务、SSE 推送、静态文件（`frontend/`、`backend/static/`）、会话与鉴权 |
| `src/jobs/` | 队列、并发上限、Job 目录创建、spawn `sigflow-tool`、JSONL 解析、状态合成、取消（杀进程组）、孤儿回收 |
| `src/devices/` | 设备租约表：`flash`/`capture` 按设备互斥，抢不到则排队；对应原 `DeviceControlLease` 语义 |
| `src/wave/` | 每用户波形 worker 的启动、JSONL 请求路由、空闲回收、VCD 打开/关闭 |
| `src/agent/` | 启动并监督 Python（bootstrap 经 stdin、退避重启、健康轮询）；实现 Python 依赖的 Gateway 兼容路由；作为 UI 调 Agent 的 `/api/v1/sessions|runs|events` |
| `src/projects/` | 工程目录、文件树、读写、快照；`userprojects/<uid>/<project>` 的唯一管理者 |
| `src/plugins/` | 网页插件注册表、能力与状态展示数据 |
| `src/util/` | `config.txt` 解析、日志、JSONL 编解码 |

#### 4.2 关键机制

- **并发**：全局与每用户上限来自 `config.txt`；超限 Job 停在 `Queued`，由 Node 排队。C++ 侧一次性进程之间无共享状态，天然并行。
- **取消**：`spawn` 时 `detached: true`，取消用 `process.kill(-pid, 'SIGTERM')`，超时后 `SIGKILL`。
- **状态机**：`Queued`（Node）→ `Running`（C++ 写入）→ 终态；Node 合并两侧状态对外呈现。
- **设备互斥**：Node 侧租约表按 `deviceId` 加锁，`flash`/`capture` 排队而非直接 `DEVICE_BUSY`；原版是进程内单 scope，这里是跨进程表。
- **崩溃恢复**：Node 记录 `(pid, jobId, startedAt)`；重启时扫描 Job 目录，把残留 `Running` 标记为 `Failed`。

#### 4.3 Python 兼容层（保持 Python 零改动）

Python Agent 是双向的：对 UI 提供 `/api/v1/sessions|runs|events`，同时作为客户端调用宿主提供的 Gateway。Node 同时扮演这两个角色：

1. **启动**：`python -m sigflow_edu_agent --bootstrap-stdin`，经 stdin 写**一行** JSON：
   `type`、`protocol`（`edu.api.v1`）、`instance_id`、`nonce`、`gateway_url`、`gateway_token`、`ui_token`、`data_directory`。
2. **Gateway 兼容路由**（当前 Python 代码实际调用的三条）：
   `GET /api/v1/health`、`GET /api/v1/projects/{id}/context`、`GET /api/v1/jobs/{id}/report`。
3. **兼容语义**（不满足会被判为 `GATEWAY_UNAVAILABLE`）：
   成功信封 `{"schema_version":"edu.api.v1","request_id":…,"trace_id":…,"data":{…}}`、`data` 必须是对象；`Authorization: Bearer <gateway_token>`；只认 `200/201/202`；`Idempotency-Key` 冲突返回 `409` + `IDEMPOTENCY_CONFLICT`；保留 `POLICY_DENIED`/`NOT_FOUND`/`STALE_REVISION`/`IDEMPOTENCY_CONFLICT`/`CURSOR_EXPIRED`/`RESOURCE_EXHAUSTED`/`CAPABILITY_UNAVAILABLE`；响应 ≤ 2 MiB；不重定向；仅 loopback。
4. **UI 侧**：Node 以 `ui_token` 调 Agent 的会话/运行/事件接口，浏览器只与 Node 通信。

`grants` / `ui-receipts`（授权签发）当前流程未使用，暂不实现；如后续启用，按 `contracts/edu-agent/v1/` 的契约补。

#### 4.4 `config.txt`

由 Node 启动时读取，C++ 侧通过命令行参数接收，不直接读文件。键分组：`[server]`（监听地址/端口）、`[jobs]`（并发上限、超时、日志/输出上限）、`[wave]`（worker 空闲回收、同时打开的 VCD 数）、`[devices]`（租约等待）、`[tools]`（工具路径覆盖）、`[agent]`（解释器、重启退避、健康轮询）。

#### 4.5 与 C++ 的对接要求

以下是 P2_HANDOFF §5 的硬性要求，Node 实现时逐条落实：

1. `spawn` 使用 `detached + pipes`，参数写完后**关闭 stdin**；
2. 每次使用**新的空目录**，遵守 `--project-id`、相对产物路径与 `reports/report.json`；
3. 只解析有界 JSONL，不解析自由文本；要处理候选 artifact、缺失 `result` 和不完整尾行；
4. 取消先 `SIGTERM` 等待落盘，超时再 `SIGKILL`；退出信号与报告一起判断；
5. 重启后扫描残留目录，自行负责恢复、队列、重试与设备租约；
6. 区分 `3`（未实现）与一次真实的执行失败。

### 5. 前端

| 目录 | 内容 |
|---|---|
| `pages/` | 主工作区、编辑器、画布、波形、任务面板等页面 |
| `scripts/` | 编辑器与语法高亮、画布交互、波形绘制、面板布局、与 Node 的 HTTP/SSE 客户端 |
| `styles/` | 主题、布局、组件样式 |
| `assets/` | 图标（原 `main/res` 266 个文件）、`canvas_elements.json` |

前端**不启动任何工具、不解析工具日志、不读 VCD**：结构树/诊断来自 `POST /api/structure`、`POST /api/lint`；波形来自 Node 的波形接口；任务进度来自 SSE。

### 6. 功能划分 → 代码位置

#### 6.1 JS 独立负责的模块

| 功能划分条目 | 原代码位置 | 新落点 |
|---|---|---|
| 网页应用界面 | `main/MainFrame.cpp`、`main/ui/*`、`main/MainMenuBar.cpp` | `frontend/pages`、`frontend/scripts`、`frontend/styles` |
| 项目管理与文件读写 | `main/ProjectStartWindow.cpp`、`main/ProjectTreePanel.cpp`、`core/src/eda-core/Project.cpp` | `backend/nodejs/src/projects/` + `frontend/` |
| RTL编辑器 | `main/SigTextEditor.cpp`、`main/VerilogManager.cpp` | `frontend/scripts/`（编辑器）；解析/诊断走 C++ |
| 结构树与属性面板 | `main/SigFlowTreePanel.cpp`、`main/SigTree.cpp`、`main/SFNPropertyPanel.cpp`、`main/PropertyPanelBuilder.cpp` | `frontend/`（渲染）；树数据来自 C++ `structure` |
| 画布编辑 | `main/CanvasElement.cpp`、`CanvasEventHandler.cpp`、`CanvasModel.cpp`、`CanvasNoteBook.cpp`、`CanvasPanel.cpp`、`CanvasTextElement.cpp`、`main/Wire.cpp` | `frontend/scripts/`；元件库 `frontend/assets/canvas_elements.json` |
| 编辑操作（撤销/重做等） | `main/MainFrame.cpp`、`main/MainMenuBar.cpp` 中的 Undo/Redo | `frontend/scripts/`（命令栈） |
| 目标板与约束配置界面 | `main/fpga/FpgaPinBindingPanel.cpp`、`main/fpga/target-profiles/*.json` | `frontend/` + `backend/static/target-profiles/`；校验走 C++ |
| FPGA工作流界面 | `main/fpga/FpgaSynthesisJobsPanel.cpp`、`NextpnrJobsPanel.cpp`、`main/BuildProgressBar.cpp` | `frontend/` + `backend/nodejs/src/jobs/` |
| 仿真工作流界面 | `main/Simulation/GuiSimulationJobProvider.cpp`、`main/BuildProgressBar.cpp` | `frontend/` + `backend/nodejs/src/jobs/` |
| 波形查看与交互 | `main/wave/WaveformGLCanvas.cpp`、`WaveformGLRenderer.cpp`、`WaveformView.cpp`、`WaveformMiniMap.cpp`、`WaveformTextLayer.cpp` | `frontend/scripts/`（绘制）；数据来自 C++ 波形 worker |
| 波形分析与对比界面 | `main/wave/WaveCompareHub.cpp`、`WaveCompareSession.cpp`、`main/debug/WaveformAligner.cpp`、`WaveformComparator.cpp` | `frontend/`（展示）；对齐/比较由 C++ 计算 |
| TraceBridge配置与工作流界面 | `main/debug/TraceBridgeWindow.cpp`、`DebugContractConfigWindow.cpp` | `frontend/`（向导/配置）；采集由 C++ |
| Agent与教学交互 | `main/agent/AgentApiClient.cpp`、`AgentServiceController.cpp`、`agent/` | `backend/nodejs/src/agent/` + `frontend/` |
| 插件面板与工具状态展示 | `main/PluginManager.cpp`、`main/ToolboxPanel.cpp` | `backend/nodejs/src/plugins/` + `frontend/` |
| 项目级展示与导出交互 | `main/fpga/FpgaYosysReport.cpp`、`NextpnrReport.cpp` | `frontend/`（展示）；数据来自 Job 目录 |

#### 6.2 C++ 独立负责的模块

| 功能划分条目 | 原代码位置 | 新落点 |
|---|---|---|
| FPGA工具链执行 | `main/jobs/{JobService,ToolJobs,PlatformProcess}.cpp`、`core/src/eda-core/JobService.cpp`、`InnerPlugin/{eda-synth-yosys,eda-pnr-nextpnr,eda-pack-gowin,eda-program-openfpgaloader}` | `src/eda-core/`、`src/plugins/*` |
| 仿真执行与波形数据服务 | `InnerPlugin/eda-sim-verilator`、`main/trace/*`、`core/src/eda-agent-gateway/WaveformService.cpp` | `src/plugins/sim-verilator/`、`src/trace/` |
| 硬件设备访问 | `main/debug/{FtdiTransport,SerialTransport,PipeTransport,JtagCaptureAdapter,JtagDebugProtocol}.cpp`、`main/platform/{SerialPort,SerialEnumerator}.cpp`、`InnerPlugin/eda-program-openfpgaloader` | `src/debug/`、`src/eda-platform/` |
| TraceBridge本机执行部分 | `main/debug/*`（42 个 wx-free 文件）、`rtl/debug/*.sv` | `src/debug/`、`backend/static/rtl/debug/` |
| 工具输入与产物处理 | `main/fpga/ArtifactValidator.cpp`、`InnerPlugin/eda-synth-yosys/{YosysArtifactValidator,YosysScriptGenerator}.cpp`、`core/src/eda-core/JobService.cpp` | `src/eda-core/`、`src/plugins/*` |
| 本机文件和进程接口 | `core/src/eda-platform/*`（POSIX 部分） | `src/eda-platform/` |
| RTL硬件描述 | `rtl/debug/*.sv` | `backend/static/rtl/debug/` |
| 现有C++工具/插件后端 | `core/src/eda-core/{PluginHost,PluginRegistry}.cpp`、`InnerPlugin/*` | `src/plugins/*` + `src/cli/` 的 `jobType` 分发表 |

### 7. 接口清单

#### 7.1 Node ↔ C++（命令行 + JSONL）

调用一律为 `spawn` 一次性进程；参数见 3.5。Node 只依赖三样东西：stdout 的 JSONL 事件、Job 目录里的文件、退出码——不解析自由格式文本（日志解析已在 C++ 内完成）。

#### 7.2 Node ↔ Python

| 方向 | 接口 |
|---|---|
| Node → Python | `--bootstrap-stdin` + 一行 JSON（8 个字段）；健康探活 |
| Python → Node（Gateway 兼容） | `GET /api/v1/health`、`GET /api/v1/projects/{id}/context`、`GET /api/v1/jobs/{id}/report` |
| Node → Python（UI 身份） | `POST /api/v1/sessions`、`GET /api/v1/sessions/{id}`、`POST /api/v1/sessions/{id}/runs`、`GET /api/v1/runs/{id}`、`GET /api/v1/sessions/{id}/events`、`POST /api/v1/shutdown` |

#### 7.3 浏览器 ↔ Node

| 分组 | 路由（示意） |
|---|---|
| 会话 | `POST /api/session`、`GET /api/session`、`POST /api/session/logout` |
| 工程 | `GET/POST /api/projects`、`GET /api/project/files`、`GET/PUT /api/project/file` |
| 任务 | `POST /api/jobs`、`GET /api/jobs`、`GET /api/jobs/{id}`、`POST /api/jobs/{id}/cancel`、`GET /api/jobs/{id}/events`（SSE）、`GET /api/jobs/{id}/artifacts/{name}` |
| 波形 | `POST /api/wave/open`、`GET /api/wave/signals`、`POST /api/wave/query` |
| 语言服务 | `POST /api/structure`、`POST /api/lint` |
| 目标板/校验 | `GET /api/targets`、`GET /api/targets/{id}/pins`、`POST /api/validate/cst` |
| Agent | `POST /api/agent/session`、`GET /api/agent/events` |
| 静态 | `/static/*`（`backend/static/`）、前端资源（`frontend/`） |

#### 7.4 共享数据

`backend/static/` 是唯一同时被 Node 与 C++ 读取的目录：`target-profiles/`（引脚表 UI + C++ 校验）与 `rtl/debug/`（TraceBridge 输入）。运行期产物（Job 目录、VCD、报告、sidecar）一律落在用户工程内，不写入 `static/`。

### 8. 构建与运行

```bash
# 配置与构建（Linux，不联网；全工程只有 web/CMakeLists.txt）
cmake -S web -B build/web -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build/web -j

# 产物
build/web/bin/sigflow-tool --help

# JS 便捷目标（CMake 只做调度，JS 依赖仍由 package.json 管理）
cmake --build build/web --target web_js_test   # node --test backend/nodejs/test
cmake --build build/web --target web_run       # node backend/nodejs/src/index.js

# 安装布局
cmake --install build/web --prefix /opt/sigflow
# /opt/sigflow/bin/sigflow-tool
# /opt/sigflow/share/sigflow/{config.txt,frontend,static,nodejs}
```

迁移期约定：`backend/cpp` 下任何模块目录为空时，CMake 会把它退化为 INTERFACE 目标并打印提示，因此**任意迁移阶段都能 configure 和构建**；随着源文件复制进来，同名目标自动变成 STATIC 库，无需改 CMake。



