# Yosys Tang Nano 9K Regression Samples

`samples.json` is the single inventory for the TODO-A baseline suite. Each entry declares its RTL files, top module, Tang Nano 9K profile and expected result. Successful samples must synthesize to a non-empty JSON netlist; failure samples must fail before a validated JSON artifact is produced.

Run the suite only after `external/fpga-tools/runtime/yosys` passes SigFlow's Yosys preflight. The first implementation deliberately keeps expected failures at the source/configuration layer so they are stable across Yosys versions.
