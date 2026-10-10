# TraceBridge Overlay vs Direct Integration Resource Comparison

Measured: 2026-09-06 17:24:28 +08:00

Configuration: GW1NR-LV9QN88PC6/I5, GW1N-9C, 27 MHz, dedicated 921600 UART, sf_micro_ila 32-bit x 1024, minimal fixed-frame protocol. Both flows use the same user logic, debug RTL, and CST pins. Both complete yosys -> nextpnr -> gowin_pack and generate .fs without board access.

| Integration | LUT4 | DFF | BSRAM | Fmax (MHz) | .fs |
| --- | ---: | ---: | ---: | ---: | --- |
| Automatic Overlay (user top as u_dut) | 499 | 319 | 2 | 103.14 | E:\EDA_Race\Cangku\new\SigFlow_FPGA\out\tracebridge_resource_compare_20260906_172421\overlay\overlay.fs |
| Manual direct top (no DebugOverlayBuilder) | 499 | 319 | 2 | 103.14 | E:\EDA_Race\Cangku\new\SigFlow_FPGA\out\tracebridge_resource_compare_20260906_172421\direct\direct.fs |
| Overlay - Direct | 0 | 0 | 0 | n/a | n/a |

## Conclusion

Overlay is an integration and maintainability mechanism, not an extra debug core. Yosys flattens the hierarchy during synthesis, so equivalent functions and constraints should use the same resources apart from placement randomness. The main resource cost is the ILA storage, UART, and selected protocol profile.

The default minimal profile provides mask/value configuration, ARM, STATUS, one-sample READ, and RESET. Use transport.protocol = full for COBS/CRC, online fingerprinting, extended triggers, or decimation.
