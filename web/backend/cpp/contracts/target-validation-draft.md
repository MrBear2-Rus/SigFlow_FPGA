# 目标板与约束校验接口草案

状态：C++ 本地实现，待 JS 和主 contributor 评审。板卡与引脚数据沿用旧版完整 JSON；`backend/static/target-profiles/` 是共享数据源。旧资料中的器件、封装和保留引脚尚需核对官方器件资料与实物，不能仅凭本校验结果判断硬件安全或位流可用。

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

`validate cst` 的 data 包含 `valid`、`diagnostics`、`io_loc_count`、`io_port_count`；诊断项含 `severity`、`message`、`line`、`content`。检查旧版支持的 IO_LOC、IO_PORT 及注释语法、引脚存在/可用和重复分配；不保证 CST 覆盖全部端口或支持所有厂商语法。

`validate pins` 的 data 包含 `valid`、`diagnostics`、`top_module`；诊断项含 `severity`、`message`、`field`、`row`（无对应行时为 -1）。沿用旧版端口、位宽、可用引脚、重复物理引脚和 IO_TYPE 规则，并补充重复端口位检查。旧规则只对部分已绑定的向量检查缺失位，不保证完全未绑定的端口被报告；消费方不得把 valid 当作完整端口覆盖证明。

这些返回字段是接口草案，跨端接入前需双方评审。输入文件大小有上限；CLI 不管理用户身份或授权，由可信 Node 负责输入选择。
