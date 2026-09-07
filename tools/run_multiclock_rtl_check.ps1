param(
    [string]$Yosys = "external\fpga-tools\runtime\yosys\bin\yosys.exe",
    [string]$OutputDirectory = "out\multiclock_rtl_check"
)

$ErrorActionPreference = 'Stop'
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$yosysPath = if ([System.IO.Path]::IsPathRooted($Yosys)) { $Yosys } else { Join-Path $repoRoot $Yosys }
$yosysPath = (Resolve-Path $yosysPath -ErrorAction SilentlyContinue).Path
if (-not $yosysPath) {
    $command = Get-Command $Yosys -ErrorAction SilentlyContinue
    if ($command) { $yosysPath = $command.Source }
}
if (-not $yosysPath) {
    throw "Yosys was not found. Install Yosys or pass -Yosys."
}

$out = Join-Path $repoRoot $OutputDirectory
New-Item -ItemType Directory -Force -Path $out | Out-Null
$ila = Join-Path $repoRoot 'rtl\debug\sf_micro_ila.sv'
$cdc = Join-Path $repoRoot 'rtl\debug\sf_cdc_control.sv'
$multi = Join-Path $repoRoot 'rtl\debug\sf_multiclock_capture.sv'

function Invoke-YosysTop([string]$top, [string[]]$sources) {
    $json = Join-Path $out ($top + '.json')
    $sourceArgs = ($sources | ForEach-Object {
        $normalized = $_ -replace '\\', '/'
        'read_verilog -sv ' + [char]34 + $normalized + [char]34
    }) -join '; '
    $script = $sourceArgs + '; hierarchy -top ' + $top +
        '; proc; check; stat; write_json ' + [char]34 +
        ($json -replace '\\', '/') + [char]34
    $log = Join-Path $out ($top + '.log')
    & $yosysPath -p $script 2>&1 | Out-File -FilePath $log -Encoding utf8
    if ($LASTEXITCODE -ne 0) { throw "Yosys structural check failed: $top. See $log." }
    if (-not (Test-Path $json)) { throw "Yosys did not generate JSON: $top" }
    $content = Get-Content -Raw $json
    if ($content -notmatch '"modules"') { throw "Yosys JSON has no modules: $top" }
    Write-Host "PASS $top"
}

Invoke-YosysTop 'sf_cdc_control' @($cdc)
Invoke-YosysTop 'sf_multiclock_capture' @($ila, $multi)
Write-Host 'MULTICLOCK RTL CHECK: ALL PASS'
