# Phase 2 接口与复现实验

本阶段基于 `415b5a2`，桌面入口仍为 `build-m0/sigflow.exe`。底部「教学与虚拟 TraceBridge」与板面、故障、串口和波形共享一个 `VirtualSessionController`。Web 暂缓。

## 版本与折叠决策

| 层 | 契约 |
|---|---|
| 电气 | P2 板声明 `sigflow.state13.v1`；旧板继续 `state-m0.v1`，旧会话不切换代数 |
| 编码 | UND=00、FLT=02、WUD=04、WLO=05、WHI=07、SUD=0C、SLO=0D、SHI=0F、ILO=15、IHI=17、PLO=1D、PHI=1F；CON=20 |
| 冻结表 | `registry/logic/state13.v1.json` 的 1024 格与编译期表逐格核对；保留完整 32 个基础索引作查表域，外部驱动只接受约定基础态和合法带 CON 派生态，拒绝保留强度及未约定 PUD |
| 次序 | 按唯一 `driver_id` 排序逐个折叠；不假设结合律、不做并行归约 |
| Ideal | Ideal 覆盖 Strong/Weak 不新生 CON；Power 对相反 Ideal 置 CON；同级相反 Ideal 得 0x36；中途 CON 可被更强 Ideal 覆盖，`sticky_conflict` 单独保留历史，最终 `contention` 反映最终字节 |
| 研究差异 | 逆向材料对 UND/FLT、Ideal 和结合律的描述存在冲突；产品以用户明确编码、FLT=02 单位元与冻结表为准 |
| 故障 | `sigflow.fault-profile.v2`；P1 四种规则及其 PCG32 流保持原契约，P2 新规则支持 permanent/transient；button_bounce 保持 input.intent intermittent |

`ResolutionExplanation` 包含完整驱动、来源、强度、电平、折叠步骤与冲突对。盲测投影将来源替换为 `observed-source-N`，只保留观测态与折叠关系。公开观测点允许出现 `pad.10` 等物理名字；不下发秘密计划里的类型、目标字段、规则名或 seed。

## 故障挂点

| Hook | 类型 | 实际作用 |
|---|---|---|
| mapping.overlay | mapping_overlay | 在 CST 不可变结果上产生有效 PadMap，运行前和激活时执行电气校验 |
| driver.transform | pull_removal | 删除指定 pull 驱动，不强制电平 |
| topology.transform | bridge | 合并等势网络并保留全部驱动，由 STATE 表自然得到 CON |
| peripheral.observe | led_polarity_reverse / common_mismatch / dead_segment | 修改器件极性或段观察模型，Pad 波形保留 |
| protocol.transform | uart_baud_offset_ppm / uart_framing_error / uart_parity_flip | 改变位期限、start/stop 或实际 parity 位；名义接收器真实采样 |

规则结构见 `registry/schemas/fault-profile.v2.schema.json`。C++ 进一步检查板卡目标、真实 PortBitRef、Bank、重复 Pad、资源冲突与时间窗口。多个不同 UART 效果可以组合；同资源冲突拒绝。学生源码和 CST 不修改。

`stuck_at/short_to_rail` 的 FPGA 驱动目标沿用 `fpga0.pad.10` 命名。P2 显式增加 PHI/PLO 电源级驱动，与相反 Strong DUT 自然合成 CON；P1 保留原 Strong rail 语义。

## Probe、采集与严格比较

| 操作 | 数据与语义 |
|---|---|
| probe.attach/read/detach | `probe_id` 和 `target`；target 可为 `port:led_n[0]`、`pad.10`、`wire.led0.a/.b`、`led0.K`。读不会写 DUT 或提交电气驱动 |
| digital-meter | Probe 的 `nominal_mv` 由有效逻辑和 Bank VCCIO 计算；FLT/CON 返回 null。界面明确为数字名义估计，不是 SPICE |
| capture.arm | `source=virtual_probe, targets[], depth, pretrigger, decimation, trigger={kind,mask,value,count}`；可附 `sample_en`、高有效 `gate`、`max_edges/max_samples` |
| trigger | mask_equal 和边沿累计命中；stall 连续未变化；timeout 表示抽取后观察样本的计数超时。这里的 timeout 不是 RTL 握手超时 intent，禁止把二者当作同一触发配置 |
| 限额 | 32 个采集位、4096 个保存样本、64 个 Probe、4096 条测量证据；默认最多观察 100 万上升沿。sample_en/gate 关闭不伪造样本 |
| 采样位置 | Mode A 为主时钟上升沿 eval 后；Mode B 为 ILA 在该上升沿采到的 eval 前值。`sample_phase` 显式标注，直接跨相位比较返回 inconclusive |
| 导出 | `capture.raw` 保留既有 32 位原始格式；`capture.metadata.json / capture.extended.json` 保留 valid_mask、绝对 tick、edge_index；VCD 由 CaptureDecoder 生成，无效位写 x，横轴用真实 tick 的相对原点 |
| WaveCompare | 相同 source_fingerprint、时钟/Pad、decimation、targets、sample_phase；按 trigger_index 相对边沿序号对齐，拒绝边沿乱序/错位，输出最早 tick/signal/value/valid 分歧 |

Mode A 是环形预触发 + 触发后窗口；Mode B 复用既有有限线性 RTL 缓冲，仅支持 pretrigger=0。后者是真实 sf_micro_ila/sf_debug_link/sf_uart_link + Verilator，通过 ITransport 位级 UART 交换 PING/GET_INFO/CONFIG/ARM/STATUS/READ_CAPTURE COBS+CRC16 帧；独立 wrapper tap 对每个回包样本逐值核对，并进入同一个 WaveCompareSession。专用调试 UART 属于影子 wrapper，不等同于板卡 Pad17/18 传输验收。

当前锁定的 Windows Verilator debug 前端会对 UTF-8 注释触发 CRT 断言。Mode B 在影子文件中仅将注释非 ASCII 字符替换为空格，保存原文件 SHA；字符串和其它 token 不改，学生原件不改。主生产工程仍遵守现有声明导出工具的源语法与编码限制。

## 课程可信边界与操作

1. 点击「开始盲测排障」选择四道课；Controller 启动可信 Python 服务，签名和逐文件哈希通过后构建真实 runner。故障面板清空并锁定，旧采集/证据不跨会话复用。
2. 「双端 Probe / 数字表」产生可信侧登记的 measurement ID。波形查询统计来自实际 trace 事件。L1→L4 按顺序解锁，后续级别要求已采集证据，提示消耗进入台账。
3. 提交根因与修复：物理题使用受控 restore；消抖题编辑状态输出的 `student_workspace_source`，再选择该工作区的 RTL。原始签名课程文件不改。不能用客户端声明“已修复”通过判题。
4. 判题在可信侧编译和执行：消抖题有 3 个独立 PCG32 派生抖动序列；时钟/UART 题恢复时钟后验证有效帧计数、坏 stop 拒收和 +1%/+2%/-2% 后续帧恢复。未修复、错定位、未知证据 ID 或越界源码不能 pass。工具失败为 inconclusive。

| 示例 | 课堂时钟 |
|---|---|
| 标准 P2 板 | 真实声明 27 MHz |
| 课程 03 | 显式 1 kHz 教学夹具，观察 10 ms 抖动 |
| 课程 04 | 显式 1 MHz / 100 kbaud 教学夹具，加快真实位级 UART 判题 |

`task.public.json` 的 clock_fixture 记录夹具与物理板 27 MHz 的差别，GUI 显示会话实际频率。完整课程包是教师部署包，不能把 private 文件随学生客户端分发；本机例子用于教学演示，不保护本机管理员免于读取磁盘。公钥在 registry，私钥只在忽略目录 `.work/p2/teacher-key.pem`；重签课程需教师持有该私钥。

网关为 loopback bearer HTTP `POST /api/v1/virtual-teaching/call`，四能力 `virtual.session.read / virtual.evidence.query / teaching.hint.request / teaching.grade.submit`，run_token 绑定会话，拒绝无 bearer、跨 run、Origin 和角色自提升。服务和 C++ Runner 均在返回前投影。Agent SDK 为 `agent/src/sigflow_edu_agent/virtual_teaching_client.py`，仅标准库；LLM 不接收私有 rubric，不执行评分。

## 复现

在固定工具链环境、Python >=3.11 下安装 `tools/virtual/requirements-phase2.txt`，先完成原有 CMake 配置。运行：

```powershell
python tools/virtual/verify_phase2.py --build-dir build-m0 --mode faults
python tools/virtual/verify_phase2.py --build-dir build-m0 --mode probes
python tools/virtual/verify_phase2.py --build-dir build-m0 --mode teaching
python tools/virtual/verify_phase2_gui.py --build-dir build-m0 --gui build-m0/tests/virtual/u1/u1_m0_full_product_acceptance.exe
python tools/virtual/verify_phase2_final.py
```

最后一个入口构建桌面、串行运行原 37 + 新 4 项 CTest，核对 M0/P1 88 份保护文件（60 份 M0 + 27 份 P1 artifact + P1 summary），仅写 P2 新归档。不要用旧 final 脚本覆盖历史证据。GUI 验收需要可交互 Windows 桌面。

联合回归发现并修复了旧 CoreJobService 的发布竞态：成功状态先于 artifacts/metrics 可见，偶发导致旧 sim.run 被读成“成功但无 VCD”。现在在同一互斥锁下发布完整成功报告；原有 job_service_smoke 增加 32 次并发轮询检查，CTest 总目标数仍为 41。历史证据不重写。
