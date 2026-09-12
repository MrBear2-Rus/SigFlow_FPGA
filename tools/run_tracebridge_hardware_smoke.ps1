param(
    [string]$Port = "COM7",
    [int]$Baud = 921600,
    [string]$Bitstream = "out/full_17_18.fs",
    [switch]$Program,
    [string]$Board = "tangnano9k",
    [int]$TimeoutMs = 1000,
    [int]$ExpectedDepth = 1024,
    [int]$ExpectedWidth = 32,
    [int]$ReadCount = -1,
    [int]$Decimation = 1,
    [switch]$SyncCalibrate,
    [switch]$VerifyDemoWaveform
)

$ErrorActionPreference = "Stop"

function Update-Crc([int]$crc, [int]$value) {
    $current = $crc -bxor (($value -band 0xFF) -shl 8)
    for ($index = 0; $index -lt 8; $index++) {
        if (($current -band 0x8000) -ne 0) {
            $current = (($current -shl 1) -bxor 0x1021) -band 0xFFFF
        } else {
            $current = ($current -shl 1) -band 0xFFFF
        }
    }
    return $current
}

function Encode-Cobs([byte[]]$payload) {
    $encoded = [System.Collections.Generic.List[byte]]::new()
    $source = 0
    while ($source -le $payload.Length) {
        $zero = $source
        while ($zero -lt $payload.Length -and $payload[$zero] -ne 0) {
            $zero++
        }
        [void]$encoded.Add([byte]($zero - $source + 1))
        for ($index = $source; $index -lt $zero; $index++) {
            [void]$encoded.Add($payload[$index])
        }
        $source = $zero + 1
    }
    return [byte[]]$encoded
}

function Decode-Cobs([byte[]]$encoded) {
    $decoded = [System.Collections.Generic.List[byte]]::new()
    $source = 0
    while ($source -lt $encoded.Length) {
        $code = $encoded[$source]
        $source++
        if ($code -eq 0) { throw "invalid COBS code" }
        for ($index = 1; $index -lt $code; $index++) {
            if ($source -ge $encoded.Length) { throw "truncated COBS frame" }
            [void]$decoded.Add($encoded[$source])
            $source++
        }
        if ($code -lt 255 -and $source -lt $encoded.Length) {
            [void]$decoded.Add([byte]0)
        }
    }
    return [byte[]]$decoded
}

function New-Frame([int]$type, [int]$sequence, [byte[]]$data) {
    $payload = [System.Collections.Generic.List[byte]]::new()
    [void]$payload.Add(1)
    [void]$payload.Add([byte]$type)
    [void]$payload.Add([byte]$sequence)
    [void]$payload.Add([byte]$data.Length)
    $payload.AddRange($data)
    $crc = 0xFFFF
    foreach ($value in $payload) {
        $crc = Update-Crc $crc $value
    }
    [void]$payload.Add([byte]($crc -band 0xFF))
    [void]$payload.Add([byte](($crc -shr 8) -band 0xFF))
    return [byte[]](@(0) + (Encode-Cobs ([byte[]]$payload)) + @(0))
}

function Read-Frame([System.IO.Ports.SerialPort]$serial) {
    $frame = [System.Collections.Generic.List[byte]]::new()
    $deadline = (Get-Date).AddMilliseconds($TimeoutMs)
    while ((Get-Date) -lt $deadline) {
        if ($serial.BytesToRead -gt 0) {
            $value = $serial.ReadByte()
            [void]$frame.Add([byte]$value)
            if ($frame.Count -gt 1 -and $value -eq 0) {
                return [byte[]]$frame
            }
        } else {
            Start-Sleep -Milliseconds 2
        }
    }
    throw "response timeout"
}

function Read-ProtocolFrame([System.IO.Ports.SerialPort]$serial) {
    $wire = Read-Frame $serial
    if ($wire.Length -lt 2 -or $wire[0] -ne 0 -or $wire[$wire.Length - 1] -ne 0) {
        throw "invalid wire frame"
    }
    $encoded = if ($wire.Length -eq 2) { [byte[]]@() } else { [byte[]]$wire[1..($wire.Length - 2)] }
    $payload = Decode-Cobs $encoded
    if ($payload.Length -lt 6) { throw "short protocol payload" }
    $length = $payload[3]
    if ($payload.Length -ne 4 + $length + 2) { throw "protocol length mismatch" }
    $crc = 0xFFFF
    for ($index = 0; $index -lt 4 + $length; $index++) {
        $crc = Update-Crc $crc $payload[$index]
    }
    $wireCrc = [int]$payload[4 + $length] -bor (([int]$payload[5 + $length]) -shl 8)
    if ($crc -ne $wireCrc) { throw "protocol CRC mismatch" }
    return [pscustomobject]@{
        Version = $payload[0]
        Type = $payload[1]
        Sequence = $payload[2]
        Data = if ($length -eq 0) { [byte[]]@() } else { [byte[]]$payload[4..(3 + $length)] }
    }
}

function Invoke-ProtocolCommand([System.IO.Ports.SerialPort]$serial, [int]$type, [int]$sequence, [byte[]]$data) {
    $frame = New-Frame $type $sequence $data
    $serial.Write($frame, 0, $frame.Length)
    $response = Read-ProtocolFrame $serial
    if ($response.Sequence -ne $sequence) { throw "sequence mismatch" }
    return $response
}

if ($Program) {
    $loader = Join-Path $PSScriptRoot "..\external\fpga-tools\runtime\openfpgaloader\bin\openFPGALoader.exe"
    $image = (Resolve-Path -LiteralPath $Bitstream).Path
    & $loader -b $Board -m $image
    if ($LASTEXITCODE -ne 0) { throw "openFPGALoader failed with exit code $LASTEXITCODE" }
}

if ($Decimation -lt 0 -or $Decimation -gt 255) { throw "Decimation must be between 0 and 255" }

$serial = [System.IO.Ports.SerialPort]::new(
    $Port, $Baud, [System.IO.Ports.Parity]::None, 8, [System.IO.Ports.StopBits]::One)
$serial.ReadTimeout = $TimeoutMs
$serial.WriteTimeout = $TimeoutMs
$serial.Open()
try {
    $serial.DiscardInBuffer()
    $sequence = 1

    if ($SyncCalibrate) {
        $preamble = [byte[]](0x55, 0xAA, 0x55, 0xAA)
        $serial.Write($preamble, 0, $preamble.Length)
        Write-Host "SYNC preamble sent (55 AA 55 AA)"
    }

    $response = Invoke-ProtocolCommand $serial 1 $sequence ([byte[]](7))
    if ($response.Type -ne 2 -or $response.Data.Length -ne 1 -or $response.Data[0] -ne 1) {
        throw "PING validation failed"
    }
    Write-Host "PING/PONG PASS"
    $sequence = ($sequence + 1) -band 0xFF

    $response = Invoke-ProtocolCommand $serial 3 $sequence ([byte[]]@())
    if ($response.Type -ne 4 -or $response.Data.Length -ne 13) {
        throw "GET_INFO validation failed"
    }
    $depth = [int]$response.Data[9] -bor (([int]$response.Data[10]) -shl 8)
    $width = $response.Data[11]
    if ($depth -ne $ExpectedDepth -or $width -ne $ExpectedWidth) {
        throw "unexpected GET_INFO geometry: ${width}x${depth}"
    }
    Write-Host "GET_INFO PASS (${width}x${depth})"
    $sequence = ($sequence + 1) -band 0xFF

    $config = [byte[]](0, 0, 0, 0, 0, 0, 0, 0, [byte]$Decimation, 0, 1, 0)
    $response = Invoke-ProtocolCommand $serial 5 $sequence $config
    if ($response.Type -ne 6) { throw "CONFIG validation failed" }
    Write-Host "CONFIG PASS (decimation=$Decimation)"
    $sequence = ($sequence + 1) -band 0xFF

    $response = Invoke-ProtocolCommand $serial 7 $sequence ([byte[]]@())
    if ($response.Type -ne 6) { throw "ARM validation failed" }
    Write-Host "ARM PASS"
    $sequence = ($sequence + 1) -band 0xFF

    $status = $null
    $statusDeadline = (Get-Date).AddMilliseconds($TimeoutMs)
    while ((Get-Date) -lt $statusDeadline) {
        Start-Sleep -Milliseconds 5
        $status = Invoke-ProtocolCommand $serial 9 $sequence ([byte[]]@())
        $sequence = ($sequence + 1) -band 0xFF
        if ($status.Type -eq 9 -and $status.Data.Length -ge 3 -and (($status.Data[0] -band 0x40) -ne 0)) {
            break
        }
    }
    if ($null -eq $status -or $status.Type -ne 9 -or $status.Data.Length -lt 3 -or (($status.Data[0] -band 0x40) -eq 0)) {
        throw "STATUS did not reach DONE"
    }
    $triggerIndex = [int]$status.Data[1] -bor (([int]$status.Data[2]) -shl 8)
    Write-Host "STATUS PASS (flags=0x$($status.Data[0].ToString('X2')) trigger_index=$triggerIndex)"

    if ($ReadCount -lt 0) { $ReadCount = $ExpectedDepth }
    if ($ReadCount -gt $ExpectedDepth) { throw "ReadCount exceeds ExpectedDepth" }
    $samples = [System.Collections.Generic.List[uint32]]::new()
    for ($address = 0; $address -lt $ReadCount; $address++) {
        $request = [byte[]]([byte]($address -band 0xFF), [byte](($address -shr 8) -band 0xFF), 1, 0)
        $response = Invoke-ProtocolCommand $serial 11 $sequence $request
        if ($response.Type -ne 12 -or $response.Data.Length -ne 8 -or
            $response.Data[0] -ne ($address -band 0xFF) -or
            $response.Data[1] -ne (($address -shr 8) -band 0xFF) -or
            $response.Data[2] -ne 1 -or $response.Data[3] -ne 0) {
            throw "READ validation failed at address $address"
        }
        $sample = [uint32]$response.Data[4] -bor
            ([uint32]$response.Data[5] -shl 8) -bor
            ([uint32]$response.Data[6] -shl 16) -bor
            ([uint32]$response.Data[7] -shl 24)
        [void]$samples.Add($sample)
        $sequence = ($sequence + 1) -band 0xFF
    }
    Write-Host "READ_CAPTURE PASS ($ReadCount samples)"
    if ($VerifyDemoWaveform) {
        if ($samples.Count -lt 2) { throw "VerifyDemoWaveform requires at least 2 samples" }
        for ($index = 0; $index -lt $samples.Count; $index++) {
            $sample = $samples[$index]
            if (($sample -shr 8) -ne 0 -or (($sample -band 0x0F) -ne (($sample -shr 4) -band 0x0F))) {
                throw "demo waveform mismatch at address ${index}: 0x$($sample.ToString('X8'))"
            }
            if ($index -gt 0) {
                $expected = ([uint32](($samples[$index - 1] + 1) -band 0x0F))
                if (($sample -band 0x0F) -ne $expected) {
                    throw "demo waveform sequence mismatch at address ${index}"
                }
            }
        }
        Write-Host "HARDWARE LOOPBACK WAVEFORM PASS ($ReadCount samples)"
    }
    Write-Host "TRACEBRIDGE HARDWARE SMOKE PASS"
} finally {
    $serial.Close()
}
