# TraceBridge 多时钟 Verilator 验收工程

这个工程验证两个相互独立的采样时钟域：

- `sf_cdc_control` 的请求/应答 toggle 和稳定 payload 跨域；
- 两个独立采样域分别采样、触发、完成和读回；
- testbench 生成 `multiclock.vcd`，便于在 GTKWave 或 TraceBridge 中查看。

这个工程是 Verilator 的多时钟接口验收。生产版 `sf_multiclock_capture` 的完整 RTL
结构仍由 `tools\run_multiclock_rtl_check.ps1` 做 Yosys 门禁；这里使用同接口语义的
小型双域采样模型，以避开当前 Verilator 版本对生产采集核完整组合结构的兼容性问题。

在仓库根目录执行（脚本默认使用仓库内的 `tools\verilator`）：

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\run_tracebridge_multiclock_smoke.ps1
```

最后出现 `TRACEBRIDGE MULTICLOCK VERILATOR PASS` 即通过。
