# run-v1 本地契约基线

状态：2026-10-31 用户确认 D03/D05/D07/D08 后形成的 C++ 本地基线。
主 contributor / Node 的实现与联调尚未验收；P2 已接入生产 run 入口和 Runtime；真实 provider 仍未迁入。

## 文件与兼容性

| 文件 | 用途 |
|---|---|
| manifest.schema.json | 旧 eda.job.v1 字段，不改变其 snake_case |
| report.schema.json | 旧 eda.jobreport.v1 字段；metrics/diagnostics 保持 provider-owned JSON |
| request.schema.json | 新 sigflow.execution-request.v1 执行请求快照 |
| event.schema.json | README 的 log/progress/diagnostic/artifact/result 事件 |
| fixtures/ | 可由 Node 独立读取的成功/失败/请求/清单/JSONL 样例 |

所有 schema 为 JSON Schema Draft 7，无外部引用，不要求联网。
版本字段或既有字段语义改变需协同更新消费者；不要通过换字段大小写暗中变更协议。
当前 envelope 禁止未声明字段；扩展需显式评审并同步 schema/样例/消费者。
旧 metrics/diagnostics 可能为 null、数组或对象，envelope 不擅自收窄。

## request.json 字段

必填键由 schema 明确；nullable 字段也需要保留键：
- schema_version：sigflow.execution-request.v1。
- job_id/job_type/plugin_id/project_id：调用与 provider 标识。
- project_path/work_dir：路径，与 project_id 分离；CLI 后续提供 --project-id。
- parameters：旧工具参数对象；不把 JobRequest 外层字段塞到这里。
- metadata：可信宿主提供的来源数据对象；首版默认空对象。
- require_confirm：旧硬件确认语义，首版默认 false；flash 确认传递方式尚未确定。
- timeout_sec：非负整数或 null；null 表示调用方未显式设置，不是工具已无限运行的证明。
- max_output_bytes：非负整数，0 表示不限制；Node 下发适合部署环境的有限值。
- tool_root/data_dir：调用选定配置，可为空；工具/资源有效解析值应在后续执行报告补充。
- input_snapshot_id：可信宿主提供的源码快照引用或 null；不从 parameters 的 hash 推导。

此文件保存执行请求，不包含源码内容。只有宿主提供不可变来源并保留工具与资源版本，
才可进行对应的可复现执行验收；input_snapshot_id=null 的样例不能被称为源码快照。

## 结果码与状态

真实工具已退出时保留其 exitCode；信号终止使用现有平台层的 128+signal 约定。
没有可用的工具退出状态（缺工具、启动失败、工具前参数/IO/内部错误）使用 -1，
由 diagnostics 给出原因，不冒充工具返回码。工具成功但产物校验失败时可保留工具 0，
同时报告 state=Failed，CLI 退出 4；消费者必须同时检查状态。

超时/取消由 Runtime 显式选择 TimedOut/Cancelled；有真实工具终止状态则保留，
未启动工具则为 -1。CLI 本身的 0/2/3/4 与 report.exit_code/result.exitCode 不同。
强杀、异常崩溃、不可写报告允许没有最终 result，Node 必须结合进程状态处理。
正常 result 必须为终态，写报告成功后发送一次。

## Artifact.path

相对当前 Job 目录，禁止绝对路径及 .. 段；与旧 provider 的绝对路径相比，
由 Runtime 在验证真实文件后转换，不能直接把绝对路径按字符串截短。
sha256 为已验证文件的小写 64 位十六进制值。
Schema 只检查表示形式，不证明文件存在、目录权限、符号链接安全或摘要真实性。
这些由 P2 Runtime/平台测试和部署隔离承担。

## 验证

C++ test_job_protocol 将实际序列化结果与 fixtures 对照，并写到构建目录的 contract-output。
test_run_schemas 使用本机已有的 Python jsonschema 验证源码样例与 C++ 输出，并检查
缺字段、非法数值、类型错误、路径逃逸、错误事件字段和非终态 result 等反例。
未安装 jsonschema 时 CMake 明确提示该检查不可用，不自动安装；不能据此宣称 schema 通过。
C++ JobProtocol 是序列化工具，不在内部执行 JSON Schema 校验；运行参数和实际文件安全
仍需 Runtime 校验，不能把 serializer 当作安全边界。

Node 交接验收：独立读取所有 fixtures，确认 snake_case 与事件 camelCase 的区别、
reports/report.json 新路径、相对 artifact 路径及退出码语义。完成后才能勾选跨端验收。

## P2 实现更新（2026-11-01）

具体调用和错误/取消语义见 ../../P2_HANDOFF.md。CLI 现已使用 --project-id，
按 Runtime 写入执行事实，静态工厂未迁入时返回 3。request.metadata 中的
`tool_root_source` 为宿主生成的配置来源，值 argument/environment/default。
该字段位于既有 metadata 对象内，不改变 envelope。provider 通过 Runtime
辅助入口使用选定配置，不靠读取用户 parameters 来推断宿主来源。
