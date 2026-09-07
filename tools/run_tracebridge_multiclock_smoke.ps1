param(
    [string]$Verilator = "",
    [string]$OutputDirectory = "out\tracebridge_multiclock_smoke"
)

$ErrorActionPreference = 'Stop'
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$verilatorArgument = $Verilator
if ([string]::IsNullOrWhiteSpace($verilatorArgument)) {
    $verilatorArgument = Join-Path $repoRoot 'tools\verilator'
}
$rtlRoot = Join-Path $repoRoot 'rtl\debug'
$fixtureRoot = Join-Path $repoRoot 'examples\tracebridge_multiclock_smoke'
$rtl = Join-Path $fixtureRoot 'rtl\multiclock_demo_top.sv'
$tb = Join-Path $fixtureRoot 'multiclock_tb.cpp'
$out = Join-Path $repoRoot $OutputDirectory
$mdir = Join-Path $out 'obj_dir'

foreach ($required in @($rtl, $tb, (Join-Path $rtlRoot 'sf_cdc_control.sv'),
        (Join-Path $fixtureRoot 'rtl\tracebridge_multiclock_domain.sv'))) {
    if (-not (Test-Path $required)) { throw "Multiclock smoke fixture is incomplete: $required" }
}

$verilatorPath = $null
if (Test-Path $verilatorArgument -PathType Leaf) {
    $verilatorPath = (Resolve-Path $verilatorArgument).Path
    if ([System.IO.Path]::GetFileName($verilatorPath) -eq 'verilator') {
        $native = Join-Path (Split-Path $verilatorPath -Parent) 'verilator_bin_dbg.exe'
        if (Test-Path $native) { $verilatorPath = (Resolve-Path $native).Path }
    }
} elseif (Test-Path $verilatorArgument -PathType Container) {
    $candidates = @(
        (Join-Path $verilatorArgument 'bin\verilator_bin_dbg.exe'),
        (Join-Path $verilatorArgument 'verilator-install\bin\verilator_bin_dbg.exe'),
        (Join-Path $verilatorArgument 'verilator-build-nmake\src\verilator_bin_dbg.exe'))
    $candidate = $candidates | Where-Object { Test-Path $_ } | Select-Object -First 1
    if ($candidate) { $verilatorPath = (Resolve-Path $candidate).Path }
}
if (-not $verilatorPath) {
    $resolved = Get-Command $verilatorArgument -ErrorAction SilentlyContinue
    if ($resolved) { $verilatorPath = $resolved.Source }
}
if (-not $verilatorPath) { throw "Verilator was not found. Install it, add it to PATH, or pass -Verilator." }

$verilatorRoot = $null
$verilatorLeaf = [System.IO.Path]::GetFileName($verilatorPath)
if ($verilatorLeaf -match '^verilator(_bin_dbg)?\.exe$') {
    $candidateRoot = Split-Path (Split-Path $verilatorPath -Parent) -Parent
    if (Test-Path (Join-Path $candidateRoot 'include')) {
        $verilatorRoot = $candidateRoot
        $env:VERILATOR_ROOT = $verilatorRoot
    }
}
if (-not $verilatorRoot -and $env:VERILATOR_ROOT) {
    $verilatorRoot = $env:VERILATOR_ROOT
}

$vsDevCmdCandidates = @(
    'D:\Program_Files\VS2022\Common7\Tools\VsDevCmd.bat',
    (Join-Path ${env:ProgramFiles} 'Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat'),
    (Join-Path ${env:ProgramFiles} 'Microsoft Visual Studio\2022\Professional\Common7\Tools\VsDevCmd.bat'),
    (Join-Path ${env:ProgramFiles} 'Microsoft Visual Studio\2022\Enterprise\Common7\Tools\VsDevCmd.bat')
)
$vsDevCmd = $vsDevCmdCandidates | Where-Object { $_ -and (Test-Path $_) } | Select-Object -First 1
if ($vsDevCmd) {
    $vsEnvironment = & cmd.exe /d /s /c "call `"$vsDevCmd`" -arch=x64 -host_arch=x64 >nul && set"
    foreach ($line in $vsEnvironment) {
        $pair = $line -split '=', 2
        if ($pair.Count -eq 2) { Set-Item -Path ("Env:" + $pair[0]) -Value $pair[1] }
    }
}
if (-not $verilatorRoot -and $env:VERILATOR_ROOT) { $verilatorRoot = $env:VERILATOR_ROOT }
if (-not $verilatorRoot) { throw "VERILATOR_ROOT could not be determined for the native Windows build." }

New-Item -ItemType Directory -Force -Path $out | Out-Null
if (Test-Path $mdir) { Remove-Item -LiteralPath $mdir -Recurse -Force }

Push-Location $out
try {
    & $verilatorPath --cc --trace --top-module multiclock_demo_top `
        --Mdir $mdir $rtl (Join-Path $rtlRoot 'sf_cdc_control.sv') `
        (Join-Path $fixtureRoot 'rtl\tracebridge_multiclock_domain.sv')
    if ($LASTEXITCODE -ne 0) { throw "Verilator build failed with exit code $LASTEXITCODE." }

    $exe = Join-Path $mdir 'Vmulticlock_demo_top.exe'
    $cppSources = @($tb,
        (Join-Path $verilatorRoot 'include\verilated.cpp'),
        (Join-Path $verilatorRoot 'include\verilated_vcd_c.cpp'),
        (Join-Path $verilatorRoot 'include\verilated_threads.cpp')) +
        @(Get-ChildItem $mdir -Filter '*.cpp' | Select-Object -ExpandProperty FullName)
    $objects = @()
    foreach ($source in $cppSources | Select-Object -Unique) {
        $object = Join-Path $mdir (([System.IO.Path]::GetFileNameWithoutExtension($source)) + '.obj')
        & cl.exe /nologo /std:c++20 /EHsc /O2 "/I$mdir" "/I$(Join-Path $verilatorRoot 'include')" `
            "/I$(Join-Path $verilatorRoot 'include\vltstd')" /c "/Fo$object" $source
        if ($LASTEXITCODE -ne 0) { throw "C++ compile failed: $source" }
        $objects += $object
    }
    & cl.exe /nologo "/Fe$exe" $objects
    if ($LASTEXITCODE -ne 0) { throw "C++ link failed." }
    if (-not (Test-Path $exe)) { throw "Native build produced no Vmulticlock_demo_top.exe." }
    & $exe
    if ($LASTEXITCODE -ne 0) { throw "Verilator multiclock replay failed with exit code $LASTEXITCODE." }

    $vcd = Join-Path $out 'multiclock.vcd'
    if (-not (Test-Path $vcd)) { throw "Multiclock program did not generate multiclock.vcd." }
    $content = Get-Content -LiteralPath $vcd -Raw
    if ($content -notmatch '\$enddefinitions' -or [regex]::Matches($content, '(?m)^#\d+\r?$').Count -lt 20) {
        throw "multiclock.vcd has incomplete waveform data."
    }
    Write-Host "VCD: $vcd"
}
finally {
    Pop-Location
}
