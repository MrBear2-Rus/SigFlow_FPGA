param(
    [string]$Port = "COM7",
    [int]$Baud = 921600,
    [string]$Bitstream = "out/full_17_18.fs",
    [int]$TimeoutMs = 1000,
    [switch]$Program
)

$ErrorActionPreference = "Stop"
$scriptPath = Join-Path $PSScriptRoot "run_tracebridge_hardware_smoke.ps1"

function Expect-Failure([string]$Name, [scriptblock]$Action) {
    try {
        & $Action
    } catch {
        Write-Host "${Name} PASS (rejected: $($_.Exception.Message))"
        return
    }
    throw "${Name} did not reject the invalid condition"
}

$programArgs = @{
    Port = $Port
    Baud = $Baud
    Bitstream = $Bitstream
    TimeoutMs = $TimeoutMs
}
if ($Program) { $programArgs.Program = $true }

& $scriptPath @programArgs -ExpectedDepth 1024 -ExpectedWidth 32 -ReadCount 1
if ($Program) { [void]$programArgs.Remove("Program") }

Expect-Failure "GEOMETRY MISMATCH" {
    & $scriptPath @programArgs -ExpectedDepth 1024 -ExpectedWidth 16 -ReadCount 4
}

Expect-Failure "TIMEOUT" {
    & $scriptPath @programArgs -ExpectedDepth 1024 -ExpectedWidth 32 -ReadCount 4 -TimeoutMs 1
}

Write-Host "TRACEBRIDGE HARDWARE NEGATIVE SMOKE PASS"
