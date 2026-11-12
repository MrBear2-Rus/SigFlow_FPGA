# 目标板与约束校验接口草案

状态：C++ 本地实现，JS 条件评审意见已纳入；板卡与引脚数据沿用旧版完整 JSON；`backend/static/target-profiles/` 是共享数据源。Tang Nano 9K 的器件封装和 QN88P 引脚表已按官方资料修正；确切板卡修订版本与各 Bank 的电压约束仍待集成方复核，不能仅凭本校验结果判断硬件安全或位流可用。

## Tang Nano 9K 静态数据来源

- QN88P 封装引脚号、I/O 名称及 Bank：Gowin [UG803E GW1NR-9 Pinout](https://cdn.gowinsemi.com.cn/UG803E.pdf) 的 QN88P 列。
- 板载按键、LED、时钟与外部 SPI Flash 网络：Sipeed [Tang Nano 9K 3672 原理图](https://dl.sipeed.com/fileList/TANG/Nano%209K/2_Schematic/Tang_Nano_9k_3672_Schematic.pdf)。LED 与时钟映射另由 Sipeed [官方 LED 示例 CST](https://raw.githubusercontent.com/sipeed/TangNano-9K-example/main/led/src/9K_LED_project.cst) 核对。
- USB-UART/JTAG 走线：Sipeed [RV-Debugger-BL702 生产版引脚说明](https://github.com/sipeed/RV-Debugger-BL702/blob/main/README.md)。原理图资料为 3672 修订版，生产版走线说明对应 3674；不同板卡修订版仍需集成方确认。

当前 `pins.json` 只有全局 `default_io_type`，没有按 Bank 表达 VCCIO；`validate cst` 与 `validate pins` 也不会核对 IO_TYPE 和 Bank 电压。Tang Nano 9K 的 Bank 3 为 1.8V，Bank 0/1/2 为 3.3V，因此 `valid=true` 不代表电压配置适配目标板。按 Bank 表达电压及其校验规则待 H-CPP-023 对齐后再扩展。

## 调用

```text
sigflow-tool target show --id <profile-id> [--data-dir <静态数据根目录>]
sigflow-tool validate cst --cst <CST 文件> --pins <板卡 pins.json>
sigflow-tool validate pins --design <旧 pin-bindings.json> --pins <板卡 pins.json> --netlist <Yosys JSON>
```

`--data-dir` 优先于 `SIGFLOW_DATA_DIR`，再使用编译配置的静态目录。`target show` 返回旧 profile 完整 JSON，不裁剪字段；id 必须与文件内 id 一致。pins 文件由调用方明确指定。

`--design` 为旧约束表，至少含 `top_module` 和 `bindings` 数组，绑定项采用旧版 `port`、`bit`、`package_pin`、`io_type` 等字段。`--netlist` 必填，是包含该顶层的 Yosys JSON；新增此参数以使端口与位宽校验有明确输入。非空的 design.device 必须与 pins.device 一致。

## JSONL 响应

正常完成查询或校验时输出一行 `{"type":"result","ok":true,"data":...}`、退出码 0。校验未通过时仍是一次成功的校验请求，由 `data.valid=false` 和 `data.diagnostics` 表示；warning 不使 valid 变为 false。参数错误退出 2，文件或解析等执行错误退出 4，并输出一行 `type=diagnostic`、`severity=error`。输入不使用 shell，也不修改源文件。

`validate cst` 的 data 包含 `valid`、`diagnostics`、`io_loc_count`、`io_port_count`；诊断项含 `severity`、`message`、`line`、`content`。`line` 为 0 起始；无法定位到源行时为 `-1`，Node 加 1 后供编辑器定位。`content` 是最相关的源文本片段：语法错误通常为整行，语义错误可为端口名或约束标识符，无对应文本时为空串。非法或不支持的 CST 语法是成功完成的校验请求（退出 0，`valid=false`）；参数错误退出 2，读取/解析执行失败退出 4。检查旧版支持的 IO_LOC、IO_PORT 及注释语法、引脚存在/可用和重复分配；不保证 CST 覆盖全部端口或支持所有厂商语法。

`validate pins` 的 data 包含 `valid`、`diagnostics`、`top_module`；诊断项含 `severity`、`message`、`field`、`row`（无对应行时为 -1）。沿用旧版端口、位宽、可用引脚、重复物理引脚和 IO_TYPE 规则，并补充重复端口位检查。旧规则只对部分已绑定的向量检查缺失位，不保证完全未绑定的端口被报告；消费方不得把 valid 当作完整端口覆盖证明。

这些返回字段是接口草案，跨端接入前需双方评审。输入文件大小有上限；CLI 不管理用户身份或授权，由可信 Node 负责输入选择。
