# P3 Yosys 综合交接

生产 `sigflow-tool run --job-type synth` 已接入 Yosys provider。支持旧参数：

| 参数 | 类型及语义 |
|---|---|
| top_module | 必填，普通 Verilog 标识符 |
| source_files | 必填，非空字符串数组；每个文件必须通过工程/静态目录授权检查 |
| strategy | 可选，baseline（默认）、debug、resource_optimized |
| yosys_family | 可选，默认 gw1n，须为合法标识符 |
| output_json | 可选；默认 artifacts/top.json，限制在当前 Job 的 artifacts 内 |
| yosys_path | 可选的显式工具路径；调用方负责只下发受信任的工具配置 |
| target_profile、target_version | 可选字符串；保留旧参数，不用于选择设备或生成脚本 |

生成 Job 内 run_yosys.ys，通过 Runtime 运行 `yosys -s`；支持 .sv 的 read_verilog -sv。
工具退出 0 后，检查 netlist JSON 有 modules/顶层/ports/cells/netnames，注册 netlist
产物，由 Runtime 计算并复核 SHA256。无产物、结构错误、工具缺失或非零退出均为 Failed。

当前测试使用独立假 Yosys 验证生产 CLI、三种策略、日志截断、错误输入、缺工具、
非零退出、缺失/损坏/错误顶层/字段的产物；它不能验证真实 Yosys 对脚本和 RTL 的支持。
当前 Ubuntu WSL 未找到 yosys；Node Web 服务仍为骨架，尚无 Job/SSE/下载实现。
因此 task.md 中的真实 Yosys 和 Node 联调项目保持未完成。安装真实 Yosys 后应按 task.md
执行真实最小 RTL 验证，并与主 contributor 的 Node 实现联调。

## Windows Yosys 互操作验证（2026-11-01）

使用用户提供的 Windows Yosys 0.49（427b5a251）通过 WSL 运行真实最小 RTL 综合。
原样调用 Windows 可执行文件失败：它无法打开 Linux `/mnt/e/...` 脚本路径。
使用仅供测试的适配器将脚本参数及脚本中的源文件/输出路径转换为 Windows 路径后，
CLI 返回 0，report 为 Succeeded，生成含 top 的 top.json（1,169,061 字节），
产物 SHA256 与报告一致。此结果验证真实 Yosys 的综合脚本及产物处理，不能视为
Linux 原生 Yosys 验收，也不能证明生产 CLI 直接支持 Windows 可执行工具。

复现（路径根据本机位置替换；该测试不会写入 Yosys 所在目录）：

```sh
python3 web/backend/cpp/tests/windows_yosys_interop.py \
  web/build/bin/sigflow-tool /path/to/windows/yosys.exe
```

适配器仅在测试临时目录内创建，不纳入生产程序或默认构建依赖。Linux 原生工具和
实际 Node 服务仍需分别验收。
