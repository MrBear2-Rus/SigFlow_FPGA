param(
    [string]$RepositoryRoot = (Split-Path -Parent $PSScriptRoot),
    [string]$BuildDirectory = 'external\fpga-tools\build\nextpnr-gowin-vs-default'
)

$buildRoot = Join-Path $RepositoryRoot $BuildDirectory
$runtimeRoot = Join-Path $RepositoryRoot 'external\fpga-tools\runtime\nextpnr'
$nextpnrExecutable = Join-Path $runtimeRoot 'bin\nextpnr-himbaechel.exe'
$chipdbSource = Join-Path $buildRoot 'share\himbaechel\gowin\chipdb-GW1N-9C.bin'
$chipdbDestination = Join-Path $runtimeRoot 'share\himbaechel\gowin\chipdb-GW1N-9C.bin'

if (-not (Test-Path -LiteralPath $nextpnrExecutable -PathType Leaf)) {
    throw "nextpnr executable was not found: $nextpnrExecutable"
}

if (-not (Test-Path -LiteralPath $chipdbSource -PathType Leaf)) {
    throw "Tang Nano 9K chip database was not found: $chipdbSource. Build the nextpnr-himbaechel-gowin-chipdb target first."
}

New-Item -ItemType Directory -Force -Path (Split-Path -Parent $chipdbDestination) | Out-Null
Copy-Item -LiteralPath $chipdbSource -Destination $chipdbDestination -Force

Write-Output "Prepared nextpnr Gowin chip database in $(Split-Path -Parent $chipdbDestination)"
