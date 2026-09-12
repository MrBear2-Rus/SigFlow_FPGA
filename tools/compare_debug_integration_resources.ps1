param(
    [string]$Yosys = "",
    [string]$Nextpnr = "",
    [string]$GowinPack = ""
)

# Compare automatic Overlay against manual direct-top integration.
# This runs yosys -> nextpnr -> gowin_pack only; it never accesses hardware.
$ErrorActionPreference = 'Continue'
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
if (!$Yosys) { $Yosys = Join-Path $repoRoot 'external\fpga-tools\runtime\yosys\bin\yosys.exe' }
if (!$Nextpnr) { $Nextpnr = Join-Path $repoRoot 'external\fpga-tools\runtime\nextpnr\bin\nextpnr-himbaechel.exe' }
if (!$GowinPack) { $GowinPack = Join-Path $repoRoot 'external\fpga-tools\runtime\apicula\Scripts\gowin_pack.exe' }
foreach ($tool in @($Yosys, $Nextpnr, $GowinPack)) {
    if (!(Test-Path $tool)) { throw "tool not found: $tool" }
}

$stamp = Get-Date -Format 'yyyyMMdd_HHmmss'
$work = Join-Path $repoRoot "out\tracebridge_resource_compare_$stamp"
New-Item -ItemType Directory -Force -Path $work | Out-Null
$rtlDir = Join-Path $repoRoot 'rtl\debug'
$userRtl = Join-Path $rtlDir 'examples\min_led_uart_top.v'

$constraints = @'
IO_LOC "clk" 52;
IO_LOC "rst_n" 16;
IO_LOC "led[0]" 10;
IO_LOC "led[1]" 13;
IO_LOC "led[2]" 14;
IO_LOC "led[3]" 15;
IO_LOC "uart_tx" 47;
IO_LOC "dbg_tx" 17;
IO_LOC "dbg_rx" 18;
IO_PORT "clk" IO_TYPE=LVCMOS33;
IO_PORT "rst_n" IO_TYPE=LVCMOS33;
IO_PORT "led[0]" IO_TYPE=LVCMOS33;
IO_PORT "led[1]" IO_TYPE=LVCMOS33;
IO_PORT "led[2]" IO_TYPE=LVCMOS33;
IO_PORT "led[3]" IO_TYPE=LVCMOS33;
IO_PORT "uart_tx" IO_TYPE=LVCMOS33;
IO_PORT "dbg_tx" IO_TYPE=LVCMOS33;
IO_PORT "dbg_rx" IO_TYPE=LVCMOS33;
'@

$debugBody = @'
    (* keep *) wire [31:0] probe_bus;
    assign probe_bus[3:0] = led;
    assign probe_bus[7:4] = led;
    assign probe_bus[31:8] = 24'b0;
    wire arm, reset_pulse;
    wire [31:0] trigger_mask, trigger_value;
    wire [7:0] decimation;
    wire [1:0] trigger_mode;
    wire [15:0] trigger_count;
    wire sample_en, busy, done, triggered;
    wire [9:0] trigger_index, rd_addr;
    wire [31:0] rd_data;
    sf_micro_ila #(.DEPTH(1024), .WIDTH(32)) u_ila (
        .clk(clk), .rst_n(dbg_rst_n), .probe(probe_bus), .arm(arm),
         .trigger_mask(trigger_mask), .trigger_value(trigger_value),
         .trigger_mode(trigger_mode), .trigger_count(trigger_count),
         .decimation(decimation), .sample_en(sample_en), .hs_valid(1'b0), .hs_ready(1'b1),
        .busy(busy), .done(done), .triggered(triggered), .rd_addr(rd_addr),
        .rd_data(rd_data), .trigger_index(trigger_index));
    sf_debug_link_minimal #(.CLK_HZ(27000000), .BAUD(921600), .WIDTH(32), .DEPTH(1024)) u_link (
        .clk(clk), .rst_n(dbg_rst_n), .rx(dbg_rx), .tx(dbg_tx),
        .arm_pulse(arm), .reset_pulse(reset_pulse),
        .trigger_mask(trigger_mask), .trigger_value(trigger_value),
        .decimation(decimation), .trigger_mode(trigger_mode), .trigger_count(trigger_count),
        .sample_en(sample_en), .busy(busy), .done(done), .triggered(triggered),
        .trigger_index(trigger_index), .rd_addr(rd_addr), .rd_data(rd_data));
'@

$overlayTop = @"
module sf_debug_overlay_top(
    input wire dbg_rx, output wire dbg_tx,
    input wire clk, input wire rst_n, output wire [3:0] led, output wire uart_tx
);
    min_led_uart_top u_dut (.clk(clk), .rst_n(rst_n), .led(led), .uart_tx(uart_tx));
    (* keep *) wire [31:0] probe_bus;
    assign probe_bus[3:0] = led;
    assign probe_bus[7:4] = led;
    assign probe_bus[31:8] = 24'b0;
    wire arm, reset_pulse;
    wire [31:0] trigger_mask, trigger_value;
    wire [7:0] decimation;
    wire [1:0] trigger_mode;
    wire [15:0] trigger_count;
    wire sample_en, busy, done, triggered;
    wire [9:0] trigger_index, rd_addr;
    wire [31:0] rd_data;
    sf_micro_ila #(.DEPTH(1024), .WIDTH(32)) u_ila (
        .clk(clk), .rst_n(1'b1), .probe(probe_bus), .arm(arm),
         .trigger_mask(trigger_mask), .trigger_value(trigger_value),
         .trigger_mode(trigger_mode), .trigger_count(trigger_count),
         .decimation(decimation), .sample_en(sample_en), .hs_valid(1'b0), .hs_ready(1'b1),
        .busy(busy), .done(done), .triggered(triggered), .rd_addr(rd_addr),
        .rd_data(rd_data), .trigger_index(trigger_index));
    sf_debug_link_minimal #(.CLK_HZ(27000000), .BAUD(921600), .WIDTH(32), .DEPTH(1024)) u_link (
        .clk(clk), .rst_n(1'b1), .rx(dbg_rx), .tx(dbg_tx),
        .arm_pulse(arm), .reset_pulse(reset_pulse),
        .trigger_mask(trigger_mask), .trigger_value(trigger_value),
        .decimation(decimation), .trigger_mode(trigger_mode), .trigger_count(trigger_count),
        .sample_en(sample_en), .busy(busy), .done(done), .triggered(triggered),
        .trigger_index(trigger_index), .rd_addr(rd_addr), .rd_data(rd_data));
endmodule
"@

function Write-Flow([string]$name, [string]$top, [string[]]$sources) {
    $dir = Join-Path $work $name
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    Set-Content -NoNewline -Path (Join-Path $dir 'board.cst') -Value $constraints
    $reads = @(
        "read_verilog -sv $($rtlDir -replace '\\','/')/sf_micro_ila.sv",
        "read_verilog -sv $($rtlDir -replace '\\','/')/sf_uart_link.sv",
        "read_verilog -sv $($rtlDir -replace '\\','/')/sf_debug_link_minimal.sv"
    )
    foreach ($source in $sources) { $reads += "read_verilog -sv $($source -replace '\\','/')" }
    $yosysScript = $reads + @(
        "hierarchy -top $top",
        "synth_gowin -family gw1n -top $top",
        "write_json $($dir -replace '\\','/')/$name.json"
    )
    Set-Content -Path (Join-Path $dir 'run.ys') -Value $yosysScript
    & $Yosys -q -s (Join-Path $dir 'run.ys') > (Join-Path $dir 'yosys.log') 2>&1
    if ($LASTEXITCODE -ne 0) { throw "${name}: yosys failed" }
    $pnrArgs = @('--json', (Join-Path $dir "$name.json"), '--write', (Join-Path $dir "$name.pnr.json"),
                 '--device', 'GW1NR-LV9QN88PC6/I5', '--vopt', 'family=GW1N-9C',
                 '--vopt', "cst=$(Join-Path $dir 'board.cst')")
    & $Nextpnr @pnrArgs > (Join-Path $dir 'nextpnr.log') 2>&1
    if ($LASTEXITCODE -ne 0) { throw "${name}: nextpnr failed" }
    & $GowinPack -d GW1N-9C -o (Join-Path $dir "$name.fs") (Join-Path $dir "$name.pnr.json") > (Join-Path $dir 'gowin_pack.log') 2>&1
    if ($LASTEXITCODE -ne 0) { throw "${name}: gowin_pack failed" }
    $log = Get-Content -Raw (Join-Path $dir 'nextpnr.log')
    $get = {
        param([string]$pattern)
        $matches = [regex]::Matches($log, $pattern)
        if ($matches.Count -eq 0) { throw "${name}: nextpnr log has no match for $pattern" }
        $matches[$matches.Count - 1].Groups[1].Value
    }
    return [pscustomobject]@{
        Name = $name
        LUT4 = & $get 'LUT4:\s*(\d+)'
        DFF = & $get 'DFF:\s*(\d+)'
        BSRAM = & $get 'BSRAM:\s*(\d+)'
        FmaxMHz = & $get "Max frequency for clock '[^']+':\s*([0-9.]+) MHz"
        FsPath = Join-Path $dir "$name.fs"
    }
}

$overlaySource = Join-Path $work 'overlay_top.sv'
Set-Content -NoNewline -Path $overlaySource -Value $overlayTop

# Direct integration uses a fixed hand-written top without DebugOverlayBuilder.
# The user module remains a child instance so its source remains untouched.
$directSources = @($userRtl, [regex]::Replace($userRtl, 'min_led_uart_top\.v$',
                                               'min_led_uart_direct_debug_top.sv'))
$directResult = Write-Flow 'direct' 'sf_debug_direct_top' $directSources
$overlaySources = @((Join-Path $rtlDir 'examples\min_led_uart_top.v'),
                    "$work\overlay_top.sv")
$overlay = Write-Flow 'overlay' 'sf_debug_overlay_top' $overlaySources

$lutDelta = [int]$overlay.LUT4 - [int]$directResult.LUT4
$dffDelta = [int]$overlay.DFF - [int]$directResult.DFF
$bramDelta = [int]$overlay.BSRAM - [int]$directResult.BSRAM
$report = @(
    '# TraceBridge Overlay vs Direct Integration Resource Comparison',
    '',
    ('Measured: ' + (Get-Date -Format 'yyyy-MM-dd HH:mm:ss K')),
    '',
    'Configuration: GW1NR-LV9QN88PC6/I5, GW1N-9C, 27 MHz, dedicated 921600 UART, sf_micro_ila 32-bit x 1024, minimal fixed-frame protocol. Both flows use the same user logic, debug RTL, and CST pins. Both complete yosys -> nextpnr -> gowin_pack and generate .fs without board access.',
    '',
    '| Integration | LUT4 | DFF | BSRAM | Fmax (MHz) | .fs |',
    '| --- | ---: | ---: | ---: | ---: | --- |',
    ('| Automatic Overlay (user top as u_dut) | ' + $overlay.LUT4 + ' | ' + $overlay.DFF + ' | ' + $overlay.BSRAM + ' | ' + $overlay.FmaxMHz + ' | ' + $overlay.FsPath + ' |'),
    ('| Manual direct top (no DebugOverlayBuilder) | ' + $directResult.LUT4 + ' | ' + $directResult.DFF + ' | ' + $directResult.BSRAM + ' | ' + $directResult.FmaxMHz + ' | ' + $directResult.FsPath + ' |'),
    ('| Overlay - Direct | ' + $lutDelta + ' | ' + $dffDelta + ' | ' + $bramDelta + ' | n/a | n/a |'),
    '',
    '## Conclusion',
    '',
    'Overlay is an integration and maintainability mechanism, not an extra debug core. Yosys flattens the hierarchy during synthesis, so equivalent functions and constraints should use the same resources apart from placement randomness. The main resource cost is the ILA storage, UART, and selected protocol profile.',
    '',
    'The default minimal profile provides mask/value configuration, ARM, STATUS, one-sample READ, and RESET. Use transport.protocol = full for COBS/CRC, online fingerprinting, extended triggers, or decimation.'
) -join [Environment]::NewLine
$reportPath = Join-Path $repoRoot 'docs\tracebridge_overlay_vs_direct_resource_comparison.md'
Set-Content -Path $reportPath -Value $report -Encoding utf8
Write-Host "Report: $reportPath"
