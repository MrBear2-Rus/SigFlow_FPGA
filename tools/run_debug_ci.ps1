param(
    [string]$VsDevCmd = "D:\Program_Files\VS2022\Common7\Tools\VsDevCmd.bat"
)

# TraceBridge 无硬件回归（T-CI-04 入口）：
# 编译并运行 P0/P1a/P1b/P2/Loopback 全部 debug 冒烟测试。
# 用法：powershell -ExecutionPolicy Bypass -File tools\run_debug_ci.ps1
$ErrorActionPreference = 'Stop'

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$outDir = Join-Path $repoRoot 'out'
New-Item -ItemType Directory -Force -Path $outDir | Out-Null

Write-Host "== run multi-clock RTL structural check"
& (Join-Path $PSScriptRoot 'run_multiclock_rtl_check.ps1')
if ($LASTEXITCODE -ne 0) { throw "多时钟 RTL 门禁失败。" }

if (-not (Test-Path $VsDevCmd)) {
    throw "VsDevCmd 未找到：$VsDevCmd（可用 -VsDevCmd 指定）"
}

$common = '/nologo /utf-8 /std:c++20 /EHsc /W3 /D _CRT_SECURE_NO_WARNINGS'
$wxDefines = '/MDd /D WXUSINGDLL /D UNICODE /D _UNICODE'
$jsonSrc = Join-Path $repoRoot '3rd\json\jsoncpp.cpp'
$mainDebug = Join-Path $repoRoot 'main\debug'
$mainJobs = Join-Path $repoRoot 'main\jobs'
$wxRoot = Join-Path $repoRoot '3rd\wxWidgets-3.2.9'
$wxIncludes = "/I `"$(Join-Path $wxRoot 'lib\vc_x64_dll\mswud')`" /I `"$(Join-Path $wxRoot 'include')`" /I `"$(Join-Path $wxRoot 'include\msvc')`""
$wxLibraries = "/link /LIBPATH:`"$(Join-Path $wxRoot 'lib\vc_x64_dll')`" wxbase32ud.lib"

function Build([string]$name, [string]$test, [string]$extraSources, [string]$includeArgs,
               [string]$linkArgs = '') {
    $exe = Join-Path $outDir "$name.exe"
    $cmd = "call `"$VsDevCmd`" -arch=x64 -host_arch=x64 >nul && cl $common $includeArgs /Fe:`"$exe`" `"$test`" $extraSources $linkArgs"
    Write-Host "== build $name"
    & cmd /c $cmd 2>&1 | Out-Null
    if ($LASTEXITCODE -ne 0) {
        throw "编译失败：$name"
    }
}

Build 'debug_p0_smoke' (Join-Path $repoRoot 'tests\debug\DebugP0Smoke.cpp') `
    "`"$mainDebug\DebugContract.cpp`" `"$mainDebug\DebugFingerprint.cpp`" `"$mainDebug\DebugSession.cpp`" `"$jsonSrc`"" `
    "/I `"$repoRoot\3rd`""
Build 'clock_domain_smoke' (Join-Path $repoRoot 'tests\debug\ClockDomainSmoke.cpp') `
    "`"$mainDebug\DebugContract.cpp`" `"$jsonSrc`"" `
    "/I `"$repoRoot\3rd`""
Build 'sf_micro_ila_smoke' (Join-Path $repoRoot 'tests\debug\SfMicroIlaModelSmoke.cpp') '' ''
Build 'sf_uart_link_smoke' (Join-Path $repoRoot 'tests\debug\SfUartLinkModelSmoke.cpp') '' `
    "/I `"$repoRoot\tests\debug`""
Build 'debug_overlay_smoke' (Join-Path $repoRoot 'tests\debug\DebugOverlaySmoke.cpp') `
    "`"$mainDebug\DebugContract.cpp`" `"$mainDebug\DebugFingerprint.cpp`" `"$mainDebug\DebugNetlistValidator.cpp`" `"$mainDebug\DebugOverlayBuilder.cpp`" `"$jsonSrc`"" `
    "/I `"$repoRoot\3rd`""
Build 'debug_loopback_smoke' (Join-Path $repoRoot 'tests\debug\DebugLinkLoopbackSmoke.cpp') `
    "`"$mainDebug\PipeTransport.cpp`"" `
    "/I `"$repoRoot\tests\debug`""
Build 'serial_smoke' (Join-Path $repoRoot 'tests\debug\SerialTransportSmoke.cpp') `
    "`"$mainDebug\SerialTransport.cpp`" `"$mainDebug\SerialPortEnumerator.cpp`" `"$mainDebug\DebugProtocol.cpp`" setupapi.lib advapi32.lib" `
    "/I `"$repoRoot`""
Build 'proto_smoke' (Join-Path $repoRoot 'tests\debug\DebugProtocolSmoke.cpp') `
    "`"$mainDebug\DebugProtocol.cpp`" `"$mainDebug\CaptureDecoder.cpp`" `"$mainDebug\PipeTransport.cpp`"" `
    "/I `"$repoRoot\tests\debug`""
Build 'minimal_proto_smoke' (Join-Path $repoRoot 'tests\debug\MinimalDebugProtocolSmoke.cpp') `
    "`"$mainDebug\DebugProtocol.cpp`"" `
    ''
Build 'decode_smoke' (Join-Path $repoRoot 'tests\debug\CaptureDecoderSmoke.cpp') `
    "`"$mainDebug\CaptureDecoder.cpp`" `"$mainDebug\DebugProtocol.cpp`" `"$repoRoot\main\trace\VcdLazyTraceSource.cpp`" `"$repoRoot\main\trace\TraceSidecarIndex.cpp`"" `
    "/I `"$repoRoot\tests\debug`""
Build 'acq_smoke' (Join-Path $repoRoot 'tests\debug\DebugAcquisitionSmoke.cpp') `
    "`"$mainDebug\DebugAcquisition.cpp`" `"$mainDebug\DebugContract.cpp`" `"$mainDebug\DebugFingerprint.cpp`" `"$mainDebug\DebugSession.cpp`" `"$mainDebug\DebugProtocol.cpp`" `"$mainDebug\CaptureDecoder.cpp`" `"$mainDebug\PipeTransport.cpp`" `"$repoRoot\main\trace\VcdLazyTraceSource.cpp`" `"$repoRoot\main\trace\TraceSidecarIndex.cpp`" `"$jsonSrc`"" `
    "/I `"$repoRoot`" /I `"$repoRoot\3rd`" /I `"$repoRoot\tests\debug`""
Build 'debug_overlay_minimal_smoke' (Join-Path $repoRoot 'tests\debug\DebugOverlayMinimalProjectSmoke.cpp') `
    "`"$mainDebug\DebugAcquisition.cpp`" `"$mainDebug\DebugContract.cpp`" `"$mainDebug\DebugFingerprint.cpp`" `"$mainDebug\DebugSession.cpp`" `"$mainDebug\DebugNetlistValidator.cpp`" `"$mainDebug\DebugOverlayBuilder.cpp`" `"$mainDebug\DebugProtocol.cpp`" `"$mainDebug\CaptureDecoder.cpp`" `"$mainDebug\PipeTransport.cpp`" `"$mainDebug\SerialTransport.cpp`" `"$mainDebug\SerialPortEnumerator.cpp`" `"$repoRoot\main\trace\VcdLazyTraceSource.cpp`" `"$repoRoot\main\trace\TraceSidecarIndex.cpp`" `"$jsonSrc`" setupapi.lib advapi32.lib" `
    "/I `"$repoRoot`" /I `"$repoRoot\3rd`" /I `"$repoRoot\tests\debug`""
Build 'waveform_comparator_smoke' (Join-Path $repoRoot 'tests\debug\WaveformComparatorSmoke.cpp') `
    "`"$mainDebug\WaveformAligner.cpp`" `"$mainDebug\WaveformComparator.cpp`" `"$mainDebug\RootCauseGraph.cpp`" `"$repoRoot\3rd\vcd\vcd.cpp`" `"$jsonSrc`"" `
    "/I `"$repoRoot\3rd\vcd`" /I `"$repoRoot\3rd`""
Build 'tracebridge_priority_smoke' (Join-Path $repoRoot 'tests\debug\TraceBridgePrioritySmoke.cpp') `
    "`"$mainDebug\ReplayScenario.cpp`" `"$mainDebug\RootCauseGraph.cpp`" `"$mainDebug\DebugMappingBuilder.cpp`" `"$mainDebug\WaveformAligner.cpp`" `"$mainDebug\WaveformComparator.cpp`" `"$repoRoot\3rd\vcd\vcd.cpp`" `"$jsonSrc`"" `
    "/I `"$repoRoot\3rd\vcd`" /I `"$repoRoot\3rd`""
Build 'behavior_summary_smoke' (Join-Path $repoRoot 'tests\debug\DebugBehaviorSummarySmoke.cpp') `
    "`"$mainDebug\DebugBehaviorSummary.cpp`" `"$jsonSrc`" `"$repoRoot\3rd\vcd\vcd.cpp`"" `
    "/I `"$repoRoot\3rd\vcd`" /I `"$repoRoot\3rd`""
Build 'fake_tool' (Join-Path $repoRoot 'tests\jobs\FakeTool.cpp') '' '' ''
Build 'job_service_smoke' (Join-Path $repoRoot 'tests\jobs\JobServiceSmoke.cpp') `
    "`"$mainJobs\JobService.cpp`" `"$mainJobs\ToolJobs.cpp`" `"$mainJobs\JobRegistry.cpp`" `"$mainJobs\PlatformProcess.cpp`" `"$mainJobs\Sha256.cpp`" `"$repoRoot\main\fpga\FpgaPackService.cpp`" `"$repoRoot\main\fpga\FpgaYosysLogParser.cpp`" `"$repoRoot\main\fpga\ArtifactValidator.cpp`" `"$repoRoot\main\fpga\NextpnrLogParser.cpp`" `"$jsonSrc`"" `
    "$wxDefines /I `"$repoRoot`" /I `"$repoRoot\3rd`" $wxIncludes" `
    "$wxLibraries"
Build 'wave_uart_lane_smoke' (Join-Path $repoRoot 'tests\wave\WaveUartLaneSmoke.cpp') `
    "`"$repoRoot\main\wave\WaveUartLane.cpp`"" `
    "/I `"$repoRoot`""
Build 'trace_query_service_smoke' (Join-Path $repoRoot 'tests\wave\TraceQueryServiceSmoke.cpp') `
    "`"$repoRoot\main\trace\TraceQueryService.cpp`" `"$repoRoot\main\trace\TraceCache.cpp`" `"$repoRoot\main\trace\TraceMemoryBudget.cpp`" `"$repoRoot\main\trace\TraceSidecarIndex.cpp`" `"$repoRoot\main\trace\VcdLazyTraceSource.cpp`"" `
    "/I `"$repoRoot`""

$tests = @(
    'debug_p0_smoke',
    'clock_domain_smoke',
    'sf_micro_ila_smoke',
    'sf_uart_link_smoke',
    'debug_overlay_smoke',
    'debug_loopback_smoke',
    'serial_smoke',
    'proto_smoke',
    'minimal_proto_smoke',
    'decode_smoke',
    'acq_smoke',
    'debug_overlay_minimal_smoke',
    'waveform_comparator_smoke',
    'wave_uart_lane_smoke',
    'tracebridge_priority_smoke',
    'behavior_summary_smoke',
    'job_service_smoke',
    'trace_query_service_smoke'
)

$failed = @()
$env:Path = "$(Join-Path $wxRoot 'lib\vc_x64_dll');$env:Path"
Push-Location $repoRoot
foreach ($t in $tests) {
    Write-Host ""
    Write-Host "== run $t"
    & (Join-Path $outDir "$t.exe")
    if ($LASTEXITCODE -ne 0) {
        $failed += $t
    }
}
Pop-Location

Write-Host ""
if ($failed.Count -eq 0) {
    Write-Host "DEBUG CI: ALL PASS"
    exit 0
}
Write-Host "DEBUG CI: FAILED: $($failed -join ', ')"
exit 1
