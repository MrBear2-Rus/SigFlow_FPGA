@echo off
chcp 65001 >nul
echo =========================================
echo  Verilator Windows 安装助手
echo =========================================
echo.

REM 检查是否以管理员权限运行
net session >nul 2>&1
if %errorLevel% neq 0 (
    echo 请以管理员权限运行此脚本！
    echo 右键点击脚本，选择"以管理员身份运行"
    pause
    exit /b 1
)

set MSYS2_PATH=C:\msys64
set MSYS2_INSTALLER=msys2-x86_64-latest.exe

REM 检查MSYS2是否已安装
if exist "%MSYS2_PATH%\msys2.exe" (
    echo [1/3] MSYS2 已安装在 %MSYS2_PATH%
    goto :INSTALL_VERILATOR
)

echo [1/3] MSYS2 未安装，需要下载安装...
echo.
echo 请手动完成以下步骤：
echo 1. 访问 https://www.msys2.org/
echo 2. 下载 msys2-x86_64-latest.exe
echo 3. 运行安装程序，安装到 %MSYS2_PATH%
echo 4. 安装完成后重新运行此脚本
echo.
pause
exit /b 1

:INSTALL_VERILATOR
echo.
echo [2/3] 正在安装 Verilator...
echo.

REM 使用MSYS2安装Verilator
"%MSYS2_PATH%\usr\bin\pacman.exe" -S --noconfirm mingw-w64-x86_64-verilator

if %errorLevel% neq 0 (
    echo.
    echo 安装失败，请检查网络连接或手动安装：
    echo 1. 打开 MSYS2 MinGW 64-bit 终端
    echo 2. 运行: pacman -S mingw-w64-x86_64-verilator
    pause
    exit /b 1
)

:CONFIG_ENV
echo.
echo [3/3] 正在配置环境变量...

REM 添加到PATH
setx PATH "%PATH%;%MSYS2_PATH%\mingw64\bin;%MSYS2_PATH%\usr\bin" /M

REM 设置VERILATOR_ROOT
setx VERILATOR_ROOT "%MSYS2_PATH%\mingw64\share\verilator" /M

echo.
echo =========================================
echo  安装完成！
echo =========================================
echo.
echo 请执行以下操作：
echo 1. 关闭所有命令行窗口
echo 2. 重新打开一个新的命令行窗口
echo 3. 运行: verilator --version
echo.
echo 如果显示版本号，说明安装成功！
echo.
pause
