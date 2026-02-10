@echo off
chcp 65001 >nul
echo =========================================
echo  Manual DLL Compiler for SigFlow
echo =========================================
echo.

set TOP_MODULE=fulladder
set CACHE_DIR=.sigflow\sim\%TOP_MODULE%
set OBJ_DIR=%CACHE_DIR%\obj_dir
set VERILATOR_ROOT=C:\msys64\mingw64\share\verilator

echo Compiling %TOP_MODULE% ...
echo.

REM 使用 MSYS2 的 g++ 编译（不需要 Visual Studio）
C:\msys64\mingw64\bin\g++.exe -shared -O2 -fPIC ^
    -o %CACHE_DIR%\%TOP_MODULE%.dll ^
    %OBJ_DIR%\*.cpp ^
    %VERILATOR_ROOT%\include\verilated.cpp ^
    %VERILATOR_ROOT%\include\verilated_vcd_c.cpp ^
    %VERILATOR_ROOT%\include\verilated_threads.cpp ^
    -I%VERILATOR_ROOT%\include ^
    -I%VERILATOR_ROOT%\include\vltstd ^
    -I%OBJ_DIR% ^
    -DVL_DLL_EXPORTS ^
    -lws2_32

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
