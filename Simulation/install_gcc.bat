@echo off
chcp 65001 >nul
echo =========================================
echo  Installing GCC for MSYS2
echo =========================================
echo.
echo 请在 MSYS2 终端中运行以下命令：
echo.
echo   pacman -S mingw-w64-x86_64-gcc
echo.
echo 或者点击任意键自动安装（需要管理员权限）
pause

C:\msys64\usr\bin\pacman.exe -S --noconfirm mingw-w64-x86_64-gcc

echo.
echo 安装完成！
pause
