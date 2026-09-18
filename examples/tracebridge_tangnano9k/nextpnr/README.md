# nextpnr work directory

The default target is the Sipeed Tang Nano 9K: GW1NR-LV9QN88PC6/I5 (GW1N-9C).

```json
{
  "fpga": {
    "yosys_path": "C:/tools/yosys/yosys.exe",
    "yosys_strategy": "baseline",
    "nextpnr_path": "C:/tools/nextpnr/nextpnr-himbaechel.exe",
    "nextpnr_args": [
      "--device", "GW1NR-LV9QN88PC6/I5",
      "--vopt", "family=GW1N-9C",
      "--json", "${yosys_json}",
      "--write", "${nextpnr_dir}/top.pnr.json"
    ],
    "gowin_pack_path": "C:/tools/apicula/Scripts/gowin_pack.exe",
    "gowin_pack_args": ["-d", "${device}", "-o", "${fs_output}", "${pnr_json}"],
    "openfpgaloader_path": "C:/tools/openfpgaloader/openFPGALoader.exe",
    "openfpgaloader_args": ["-b", "tangnano9k", "${bitstream}"]
  }
}
```

`${yosys_json}`, `${nextpnr_dir}`, `${pnr_json}`, `${fs_output}` and `${device}` are replaced by SigFlow at launch. Add a `--vopt` `cst=<constraints.cst>` argument when a board constraint file is available. Run Apicula `gowin_pack -d GW1N-9C` on the PnR JSON to create a downloadable `.fs` bitstream.
