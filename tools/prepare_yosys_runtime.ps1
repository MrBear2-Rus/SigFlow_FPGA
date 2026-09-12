param(
    [string]$RepositoryRoot = (Split-Path -Parent $PSScriptRoot)
)

$sourceRoot = Join-Path $RepositoryRoot 'external\fpga-tools\src\yosys\techlibs'
$runtimeShare = Join-Path $RepositoryRoot 'external\fpga-tools\runtime\yosys\share'

if (-not (Test-Path (Join-Path $sourceRoot 'gowin\cells_sim.v'))) {
    throw "Yosys Gowin sources were not found under $sourceRoot."
}

New-Item -ItemType Directory -Force -Path (Join-Path $runtimeShare 'gowin') | Out-Null
Copy-Item -Path (Join-Path $sourceRoot 'gowin\*') -Destination (Join-Path $runtimeShare 'gowin') -Recurse -Force
foreach ($fileName in @('mul2dsp.v', 'techmap.v', 'abc9_map.v', 'abc9_model.v', 'abc9_unmap.v')) {
    Copy-Item -Path (Join-Path $sourceRoot "common\$fileName") -Destination (Join-Path $runtimeShare $fileName) -Force
}

Write-Output "Prepared Yosys Gowin share files in $runtimeShare"
