# TraceBridge Verilator 重放 Smoke

这是 2 号工作项的最小真实验收工程，用于确认仓库内置 Verilator 能够编译 DUT、执行回放 testbench 并生成可加载的 `replay.vcd`。

在仓库根目录执行：

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\run_tracebridge_replay_smoke.ps1
```

通过条件是最后输出 `TRACEBRIDGE VERILATOR REPLAY PASS`，并且 `out\tracebridge_replay_smoke\replay.vcd` 包含 `clk`、`rst_n`、`data`、`state`。如需使用其他版本，可通过 `-Verilator` 显式传入可执行文件或安装目录。
