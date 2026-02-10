@echo off
chcp 65001 >nul
echo =========================================
echo  Manual DLL Compiler (Visual Studio)
echo =========================================
echo.

REM 切换到项目目录（重要！避免在VS目录下编译）
cd /d "%~dp0\.."
echo Working directory: %cd%
echo.

set TOP_MODULE=fulladder
set CACHE_DIR=.sigflow\sim\%TOP_MODULE%
set OBJ_DIR=%CACHE_DIR%\obj_dir
set VERILATOR_ROOT=C:\msys64\mingw64\share\verilator

echo Searching for Visual Studio...

set "VCVARS_PATH="
if exist "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" (
    set "VCVARS_PATH=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
) else if exist "C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat" (
    set "VCVARS_PATH=C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat"
) else if exist "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat" (
    set "VCVARS_PATH=C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat"
)

if "%VCVARS_PATH%"=="" (
    echo ERROR: Visual Studio 2022 not found!
    echo Please install Visual Studio 2022 with C++ desktop development.
    pause
    exit /b 1
)

echo Found: %VCVARS_PATH%
echo.
echo Compiling %TOP_MODULE% ...
echo.

REM 调用 vcvars 设置环境，然后编译
call "%VCVARS_PATH%" && cl /LD /O2 /MD /EHsc /W3 ^
    /Fe"%CACHE_DIR%\%TOP_MODULE%.dll" ^
    /Fo"%OBJ_DIR%\\" ^
    "%OBJ_DIR%\*.cpp" ^
    "%VERILATOR_ROOT%\include\verilated.cpp" ^
    "%VERILATOR_ROOT%\include\verilated_vcd_c.cpp" ^
    "%VERILATOR_ROOT%\include\verilated_threads.cpp" ^
    "Simulation\sc_time_stub.cpp" ^
    /I"%VERILATOR_ROOT%\include" ^
    /I"%VERILATOR_ROOT%\include\vltstd" ^
    /I"%OBJ_DIR%" ^
    /link /DLL /MACHINE:X64 ws2_32.lib

if %errorLevel% == 0 (
    echo.
    echo =========================================
    echo  SUCCESS! DLL compiled.
    echo  Location: %CACHE_DIR%\%TOP_MODULE%.dll
    echo =========================================
) else (
    echo.
    echo =========================================
    echo  FAILED! Check errors above.
    echo =========================================
)

pause
