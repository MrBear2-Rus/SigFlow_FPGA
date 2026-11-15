param(
    [string]$MingwRoot = 'E:/sigflow_fpga/toolchain/mingw64',
    [string]$CMake = '',
    [string]$BuildDir = 'build-m0'
)
$ErrorActionPreference = 'Stop'
$projectRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
if (-not $CMake) { $CMake = Join-Path $projectRoot '.work/cmake-python/cmake/data/bin/cmake.exe' }
$env:PATH = "$MingwRoot/bin;$env:PATH"
$taskTemp = Join-Path $projectRoot '.work/temp'
New-Item -ItemType Directory -Force -Path $taskTemp | Out-Null
$env:TEMP = $taskTemp
$env:TMP = $taskTemp
$wxRoot = Join-Path $projectRoot '3rd/wx-mingw'
$cmakeArgs = @('-S', $projectRoot, '-B', (Join-Path $projectRoot $BuildDir), '-G', 'MinGW Makefiles',
    "-DCMAKE_TOOLCHAIN_FILE=$projectRoot/cmake/toolchains/mingw-gcc.cmake",
    "-DMINGW_ROOT=$MingwRoot", "-DCMAKE_MAKE_PROGRAM=$MingwRoot/bin/mingw32-make.exe",
    '-DCMAKE_BUILD_TYPE=Release', '-DSIGFLOW_USE_LOCAL_WX=OFF', '-DSIGFLOW_FETCH_WX=OFF',
    '-DSIGFLOW_ALLOW_FETCH=OFF', "-DSIGFLOW_WX_ROOT=$wxRoot",
    "-DSIGFLOW_WX_CONFIG_DIR=$wxRoot/lib/wx/include/msw-unicode-3.2", "-DSIGFLOW_WX_LIB_DIR=$wxRoot/lib",
    '-DSIGFLOW_BUILD_DEEPSEEK_PLUGIN=OFF', '-DSIGFLOW_EMBED_LOCAL_MODEL=OFF')
& $CMake @cmakeArgs
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed: $LASTEXITCODE" }
