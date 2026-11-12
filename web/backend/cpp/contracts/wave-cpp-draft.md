# 单次波形查询 C++ 草案

```text
sigflow-tool wave signals --vcd <绝对路径> [--offset <非负整数>] [--limit <1..1000>]
sigflow-tool wave query --vcd <绝对路径> --signals <单个数字信号 ID> --begin <uint64> --end <uint64> [--offset <非负整数>] [--limit <1..1000>]
```

`--signals` 当前只接受一个信号 ID；ID 取自同一 VCD 的 signals 结果。begin/end 为 VCD 原始时间刻度，闭区间。文件必须是绝对路径下的普通文件，不允许符号链接与 `..`，最大 64 MiB；调用者负责授权文件访问。每次调用重新加载 VCD，适合小规模数据和联调，非会话缓存接口。

`time_range` 是 VCD 中首次观察到的时间戳与末次观察到的时间戳；若尚无时间戳或数值事件则为 `null`。

成功输出一行 `{"type":"result","ok":true,"data":{...}}`，退出 0；错误输出一行 error diagnostic，退出 4；其他未实现命令退出 3。结果 JSONL 最大 900 KiB，超限时要求缩小分页。offset 默认 0、limit 默认 100。分页参数与 ID 为 JSON 整数；全部时间戳和区间端点使用十进制字符串，避免 JavaScript 精度丢失。

signals 的 data 包括 `schema=sigflow.wave.signals.v1`、`timescale`、可空的 `time_range`、`signals`、`offset` 和可空的 `next_offset`；信号包含 `id/name/scope/full_name/width`。query 的 data 包括 `schema=sigflow.wave.query.v1`、`signal_id/begin/end`、`initial_value`、`transitions`、`offset/next_offset`；每个跳变包含字符串 `time` 和二进制字符串 `value`。`initial_value` 是严格早于 begin 的最后一个值，没有则为 null；`transitions` 是闭区间 `[begin, end]` 内的全部跳变。同一时间的多个跳变按原文件顺序分页。

## `wave serve` 本地提案

进程由 Node 按用户会话启动，逐行从 stdin 读取 JSON 请求、逐行向 stdout 输出响应；EOF 退出，不自行管理用户会话。请求为 JSON object，`id` 是 1–128 字符的非空字符串或非负整数，`op` 为操作名。响应保留同一个 `id`，使用 `{"id":...,"ok":true,"data":...}`；错误使用 `{"id":...,"ok":false,"error":{"code":"...","message":"..."}}`。

```jsonl
{"id":"o1","op":"open","vcd":"/absolute/project/run.vcd"}
{"id":"s1","op":"signals","offset":0,"limit":100}
{"id":"q1","op":"query","signal_id":0,"begin":"0","end":"1000","offset":0,"limit":100}
{"id":"c1","op":"close"}
```

`open` 成功时返回 `schema`、`timescale`、`time_range` 和 `signal_count`；仅在文件完整加载成功后替换当前 trace，加载失败时保留已打开的 trace。`signals/query` 在尚未 open 时返回 `NOT_OPEN`；`close` 清除当前 trace。`begin/end` 必须是十进制字符串；offset/limit 为非负 JSON 整数，limit 默认为 100，范围 1–1000。每行请求上限 64 KiB、每行响应上限 900 KiB；超长请求返回错误后继续读下一行。查询结果超限返回 `RESPONSE_TOO_LARGE`，调用方应缩小分页；VCD 不存在返回 `FILE_NOT_FOUND`，其他无效请求返回 `INVALID_REQUEST`。Node HTTP 层负责映射这些错误码。C++ 串行处理请求，生命周期由 Node 管理。

只支持数字值与 VCD 的数字子集，不支持 real/string。单个 trace 在内存中加载；信号数、展开数据量、单个 token（64 KiB）、指令参数（8 项）和作用域深度（128 层）均有限制；注释按 token 流式跳过。
