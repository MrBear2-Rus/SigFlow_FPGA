---
name: gf-synth
description: >
  Synthesize SystemVerilog/Verilog with Yosys. Reports area, timing,
  and resource utilization. Warns about unsupported SV constructs.
  Example: "synthesize my design for iCE40"
allowed-tools:
  - Bash
  - Read
  - Write
  - Glob
  - Grep
  - Task
---
# GF-Synth — Yosys Synthesis Skill (Windows)

## Tool Detection

检测 Yosys 是否可用（Windows PowerShell / CMD 通用）：

```
where yosys
```

若未找到，尝试 PowerShell：

```powershell
Get-Command yosys -ErrorAction SilentlyContinue
```

**Windows 安装方式**：
- 从 [YosysHQ Release](https://github.com/YosysHQ/yosys/releases) 下载 Windows 预编译包
- 或使用包管理器：`winget install YosysHQ.Yosys` 或 `scoop install yosys`
- 解压后将 `yosys.exe` 所在目录添加到系统 `PATH` 环境变量

```
---GATEFLOW-RESULT---
STATUS: ERROR
DETAILS: Yosys not installed or not in PATH.
  1. Download from: https://github.com/YosysHQ/yosys/releases
  2. Extract and add bin/ to PATH
  3. Verify with: where yosys
---END-GATEFLOW-RESULT---
```

## Pre-Synthesis SV Subset Check

在综合前，扫描 RTL 文件中 Yosys 不支持的 SystemVerilog 高级构造：

**Windows CMD / PowerShell（使用 findstr）**：

```cmd
findstr /r /i /s "interface\  modport\  class\  bind\ " <files>
```

**PowerShell 原生**：

```powershell
Select-String -Path <files> -Pattern "^\s*interface\s|^\s*modport\s|^\s*class\s|^\s*bind\s"
```

如果检测到不兼容构造，**警告用户并建议降级或排除**，因为 Yosys 会产生难以理解的错误。

## Windows 环境注意事项

1. **路径分隔符**：Yosys 接受正斜杠 `/` 和反斜杠 `\`，但建议统一使用正斜杠以避免转义问题。可在调用前用 `Replace("\", "/")` 转换。

2. **Yosys 可执行文件名**：Windows 下通常为 `yosys.exe`，在脚本中调用时需包含 `.exe` 后缀或确保 PATH 配置正确。

3. **文件编码**：确保 Verilog/SystemVerilog 源文件使用 UTF-8 编码保存，避免中文导致解析失败。

4. **工作目录**：建议使用短路径（无空格、无中文）作为 Yosys 工作目录，避免路径解析问题。

5. **进程管理**：Windows 下 Yosys 不直接 fork 子进程，如需取消综合，使用 Ctrl+C 或在任务管理器中结束 `yosys.exe`。

## Workflow

1. **检查项目配置** — 读取 `sigflow.project` 获取 `fpga.*` 设置和 `build.top_module`
2. **工具预检** — 确认 `yosys.exe` 可用，检查运行时依赖
3. **SV 子集检查** — 扫描 `interface`/`modport`/`class`/`bind` 等不支持的构造
4. **生成 Yosys 脚本** — 拼接 `read_verilog`、`hierarchy -check`、`synth_*`、`stat` 等命令
5. **执行综合** — 调用 `yosys.exe -s <script>` 运行综合，捕获 stdout/stderr
6. **解析报告** — 从 `stat` 输出中提取 LUT/FF/BRAM/DSP 资源统计
7. **结构化输出** — 按标准格式报告综合结果

## Result Format

```
---GATEFLOW-RESULT---
STATUS: PASS | FAIL | ERROR
RESOURCES:
  LUTs: N
  FFs: N
  BRAM: N
  DSP: N
TARGET: ice40 | ecp5 | gowin | xilinx | generic
FILES: [synth output files]
DETAILS: [summary or error explanation]
---END-GATEFLOW-RESULT---
```

## 目标板映射参考

| 目标板 | Yosys synth 命令 |
|---|---|
| iCE40 (Lattice) | `synth_ice40` |
| ECP5 (Lattice) | `synth_ecp5` |
| Gowin (Tang Nano) | `synth_gowin -family GW1N-9C` |
| Xilinx 7-Series | `synth_xilinx` |
| 通用 (generic) | `synth` |
