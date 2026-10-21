# Sidecar 限额、并发与压力矩阵

> 执行日期：2026-10-22  
> 平台：Windows 11 x64 / AMD64   
> 范围：`agent/src/sigflow_edu_agent/` 的 sidecar 运行边界；不包含 Gateway、VCD 读取和磁盘持久化。

本矩阵只记录实际执行过的结果。没有证据的场景写 `SKIP` 或 `N/A`，不写成 `PASS`。

## 1. 矩阵

| 场景 | 结果 | 证据或原因 |
|---|---|---|
| 请求体正好 1 MiB | PASS | `test_request_body_of_exactly_one_mib_is_accepted` 返回 201 |
| 请求体 1 MiB + 1 字节 | PASS | `test_request_body_one_byte_over_one_mib_is_413` 返回 413 `RESOURCE_EXHAUSTED` |
| 响应超过 2 MiB | PASS | `test_response_over_two_mib_is_replaced_by_429` 直接验证响应写入护栏，返回 429 `RESOURCE_EXHAUSTED` |
| 32 会话 LRU | PASS | `test_lru_evicts_the_least_recently_used_session`、`test_default_session_cap_is_thirty_two` |
| 单会话 64 run 保留 | PASS | `test_run_cap_keeps_the_newest_sixty_four` |
| 单会话 1000 事件保留 | PASS | `test_event_cap_keeps_newest_one_thousand_and_expires_old_cursor` |
| 单次运行 16 卡上限 | PASS | `test_large_diagnostic_window_is_capped_at_sixteen_cards`、`test_card_cap_records_omissions` |
| 64 位波形 tick | PASS | `test_wave_tick_accepts_uint64_strings_and_integers_but_requires_string_responses`（uint64 上界与越界拒绝） |
| 计数超过 2^53 | PASS | `test_high_watermark_and_state_version_stay_decimal_beyond_2_53`，事件与 `state_version` 仍为十进制字符串 |
| 中文、空格、长 revision/top | PASS | `test_unicode_space_and_long_revision_are_round_tripped`，256 字符 project id、中文空格 revision/top |
| 文件系统长路径工程 | SKIP / N/A | Agent 只接收有界 opaque `project_id`/`revision`，不接收文件系统路径；路径与源文件解析属于 Gateway/S1 |
| 大 VCD / 波形窗口 | SKIP / N/A | Agent 不读取 VCD，也没有波形路由；该场景属于 Gateway/S1/S2 |
| 大报告窗口（20 条诊断） | PASS | `test_large_diagnostic_window_is_capped_at_sixteen_cards`，保留 16 卡并记录 omitted |
| 两个工程并发请求 | PASS | `test_two_projects_can_run_in_parallel`，两个会话并发返回不同 run id |
| 端口冲突 | PASS | `test_occupied_port_exits_three_with_empty_stdout`，启动退出码 3 且 stdout 为空 |
| 数据目录不可写 | SKIP / N/A | sidecar 运行期只使用内存，不写 `data_directory`；因此没有可执行的磁盘写失败路径 |
| 磁盘满 | SKIP / N/A | 同上，Agent 不持久化会话、run 或事件；磁盘满属于 Gateway/宿主职责 |
| 模型慢/断网与 Gateway 慢/断网同时发生 | PASS | `test_model_and_gateway_outages_are_survivable_together`：explain 降级为规则卡，report_review 返回 503，无伪造卡片 |

## 2. 执行命令

```powershell
cd D:\SigFlow\SigFlow_CMake\agent
python -m unittest tests.test_stress_matrix -v
python tests/run_all.py
```

## 3. 边界说明

- 2 MiB 响应护栏通过直接调用响应写入函数验证，不需要真的把 2 MiB JSON 经 loopback 发送一遍。
- 大 VCD、文件系统长路径、数据目录和磁盘属于 Gateway 或宿主职责；在 U1 sidecar 侧标为 `SKIP / N/A`，不把其他组件的测试结果记到本矩阵。
- Windows 上 SIGTERM 测试仍按既有说明跳过；该跳过项不影响本矩阵的限额与并发结论。
