# Verilator 仿真接口草案

状态：C++ 本地实现，JS 条件评审意见已纳入，待确认冻结及主 contributor 评审；共用 envelope 与事件见 [run-v1](README.md)。

## sim.build 参数

- `top_module`：必填，普通模块标识符；`source_files`：必填、非空数组，元素为工程内或获准静态资源内的源文件路径。
- `testbench`：可选普通模块标识符。旧版简化解析器从源文件中提取时钟与部分激励，生成驱动顶层的 C++ 测试程序。仅支持简单命名端口实例、单信号 `reg`/`wire` 声明、`initial begin/end` 中的延时、赋值、`@(posedge/negedge signal)`、`$display`/`$finish`，以及 `forever` 中 `#N clk = ~clk` 形式的时钟；不支持完整 Verilog 表达式、控制流或事件调度语义，也不应被当作通用仿真器。
- `out_dir`：可选，工作路径限制在当前 Job 内，默认 `sim`；`executable`：可选，限制在当前 Job 的 `artifacts` 内，默认 `artifacts/sim_main`；`verilator_path`：可选工具路径覆盖。
- 成功时登记 `sim-executable`，schema 为 `eda.sim.verilator-executable.v1`。构建使用本机 Verilator、make、C++ 编译器等环境，真实工具链兼容性需要部署环境验收。

## sim.run 参数

- `sim_exe`：必填，由可信 Node 从同工程成功的 `sim.build` 产物中选定。C++ 检查构建任务和工程身份、成功状态、产物类型、路径及摘要；可写工程中的报告和摘要本身不能证明程序来源可信。
- `duration_ticks`：可选十进制数字组成的 uint64 字符串（允许前导零及 `0`，不接受符号、空白或指数格式）；运行时校验上限并规范化为无前导零的十进制字符串，再作为生成驱动程序的时间步上限。`build_input_fingerprint`：可选字符串，原样写入指标，不用于授权。
- `working_dir`：可选，限制在当前 Job 的 `artifacts` 内，默认该目录；生成程序向工作目录的 `waveform/wave.vcd` 写入。`vcd_out`：可选，限制在当前 Job 的 `artifacts` 内；指定后必须指向运行生成的文件。
- 成功时登记 `waveform`，schema 为 `eda.wave.vcd.v1`。当前只检查文件非空和摘要，波形内容与功能正确性需实际工具验收。

路径不能穿越或包含符号链接。构建和运行均使用现有进程取消、超时、日志与报告约定。当前本地测试使用假工具，并不证明原生 Verilator、复杂 testbench 或硬件结果正确。
