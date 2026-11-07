# TraceBridge devices/capture 接口草案

稳定格式沿用旧 C++ 的 snake_case 字段。

## `devices list`

命令成功时向 stdout 输出一行 JSON：

```json
{"type":"result","ok":true,"data":{"schema":"sigflow.devices.list.v1","platform":"linux","capabilities":{"serial":true,"ftdi":false},"serial_ports":[],"ftdi_probe":{"compiled":false,"vendor_id":"0403","product_id":"6010","device_present":null}}}
```

`serial_ports[]` 项包含 `id`、`path`、`name`、`interface_number`、`vendor_id`、`product_id`、`usb_serial`。未知 USB 信息使用 `null`。`ftdi_probe.device_present` 在未编译 libftdi 时为 `null`；已编译时表示是否发现支持的 FTDI 型号，不保证区分同型号设备。枚举结果是候选路径，不保证端口可打开。该命令不打开设备、不执行采集，也不代表 Node 已取得设备租约。参数错误通过 diagnostic 输出，退出码为 2。

## Capture run 持久化记录

一次 run 在唯一目录中写入 `run.json`、`diagnostics.json` 与 `windows.jsonl`。目录下每个 attempt 使用独立子目录；产物路径以 POSIX 风格相对 run 目录记录。完整窗口必须包含普通文件 `raw_path` 和 `vcd_path`，可选 `metadata_path`。

`run.json` 顶层字段为 `schema_version`、`created_at`、可选 `updated_at`、`plan` 与 `result`。`plan` 包含 `run_id`、`output_root`、`device_id`、`contract_snapshot`、`runtime_config_hash`、`target_count`、`max_attempts`、`stop_after_failures`、`total_timeout_ms`、`attempt_timeout_ms` 和 `inter_attempt_delay_ms`。`result` 包含 `schema_version`、`run_id`、`state`、`attempts`、`completed_windows`、`timeout_count`、`failure_count` 与 `issues`。

`diagnostics.json` 包含 `schema_version`、`run_id`、`state`、`generated_at`、`plan_id`、汇总计数 `summary`、run 级 `issues` 和带 `attempt_id`、`attempt_number` 的 `attempt_issues`。每项 issue 使用 `code`、`stage`、`severity`、`observed`、`action` 与 `evidence`。

`windows.jsonl` 每行记录一次 attempt：`attempt_number`、`attempt_id`、`started_at`、`completed_at`、`elapsed_ms`、`state`、`error`、`retry_allowed`、`timing`、`transport_stats`、`protocol_stats`、`window` 与 `issues`。window 包含 `capture_id`、`session_id`、raw/VCD/metadata 相对路径、`completeness`、`identity`、`data_hash`、`duplicate_of`、`classification`、`sample_count`、`trigger_index` 和采集时间基信息。`timebase` 在 metadata 产物存在时包含 `time_domain`、`vcd_timescale`、`sample_clock_hz`、`decimation`、`trigger_observed` 与可空的 `trigger_time`。

Run 状态为 `Created`、`Validating`、`Running`、`Paused`、`Stopping`、`Completed`、`Cancelled`、`Failed`；attempt 状态为 `Planned`、`Running`、`Captured`、`TimedOut`、`Failed`、`Cancelled`。完整窗口以 `Captured` 且 `completeness=complete` 表示。`retry_allowed` 只表示 executor 已确认下一次 ARM 安全，不代表 Node 必须重试。
