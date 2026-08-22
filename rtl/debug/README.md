# rtl/debug — TraceBridge 采集核

## 文件

- `sf_micro_ila.sv`：精简采集核（P1a）。单模块，环形采样 + 掩码触发 +
  触发位置冻结 + 顺序读出；预触发重排由宿主完成。
- `sf_micro_ila_tb.sv`：自动检查 testbench（需要 Verilator/iverilog 运行，
  当前仓库无仿真器时由 yosys 做综合检查、由行为等价 C++ 测试验证语义）。

## 接口要点

- `arm`：拉高一拍启动采集。
- `trigger_mask` / `trigger_value`：掩码相等触发；`mask=0` 立即触发。
- `trigger_index`：触发样本的环形地址；宿主按
  `rel_time_k = mem[(trigger_index + k) % DEPTH]` 重排。
- `DEPTH` / `WIDTH` 参数化（默认 1024x32），存储为 Gowin BSRAM
  （`ram_style="block"`，yosys synth_gowin 映射为 SDPX9B）。
- 资源实测：1024x32 约 2 块 BSRAM + 少量 LUT/DFF（见实现记录）。
