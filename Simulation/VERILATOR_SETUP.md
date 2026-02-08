# Verilator Windows 安装指南

## 📋 概述

本指南介绍如何在 Windows 环境下安装 Verilator，以便 SigFlow 的仿真功能可以正常工作。

> ⚠️ **重要提示**: vcpkg 目前不提供 Verilator 包，请使用以下推荐的安装方法。

---

## 推荐方法：使用 MSYS2 安装

### 步骤 1：下载并安装 MSYS2

1. 访问官网下载安装程序：
   **https://www.msys2.org/**

2. 下载 `msys2-x86_64-latest.exe`

3. 运行安装程序，按提示安装（建议安装到 `C:\msys64`）

### 步骤 2：更新 MSYS2

安装完成后，打开 **MSYS2 MSYS** 终端（从开始菜单找到），执行：

```bash
# 更新包数据库和核心系统包
pacman -Syu

# 如果提示关闭终端，请关闭后重新打开，再次执行：
pacman -Su
```

### 步骤 3：安装 Verilator

在 MSYS2 终端中执行：

```bash
pacman -S mingw-w64-x86_64-verilator
```

### 步骤 4：配置环境变量

#### 添加到系统 PATH：

1. 按 `Win + R`，输入 `sysdm.cpl`，回车
2. 高级 → 环境变量 → 系统变量 → 找到 Path → 编辑 → 新建
3. 添加以下路径：

```
C:\msys64\mingw64\bin
C:\msys64\usr\bin
```

#### 添加 VERILATOR_ROOT：

在系统变量中点击 **新建**：
- 变量名：`VERILATOR_ROOT`
- 变量值：`C:\msys64\mingw64\share\verilator`

### 步骤 5：验证安装

**关闭所有命令行窗口**，然后重新打开 cmd：

```cmd
verilator --version
```

应该显示类似：
```
Verilator 5.006 2023-... rev ...
```

---

## 简化方法：使用安装脚本

我们提供了一个批处理脚本，可以自动完成大部分安装步骤：

1. 先手动下载并安装 MSYS2（见步骤 1）
2. 双击运行 `install_verilator.bat`
3. 脚本会自动安装 Verilator 并配置环境变量

---

## 替代方法：从源码编译

如果 MSYS2 方式有问题，你可以从源码编译 Verilator：

```bash
# 在 MSYS2 终端中
git clone https://github.com/verilator/verilator
cd verilator
git checkout stable

# 配置并编译
autoconf
./configure --prefix=/mingw64
make -j$(nproc)
make install
```

---

## 常见问题

### 1. "verilator 不是内部或外部命令"

- 确保已正确添加到 PATH
- 确保使用的是 **mingw64** 路径，不是 msys 路径
- **重新打开**命令行窗口（环境变量需要新窗口才能生效）

### 2. 找不到头文件（`verilated.h` 等）

确保设置了 `VERILATOR_ROOT` 环境变量：
```
VERILATOR_ROOT = C:\msys64\mingw64\share\verilator
```

### 3. MSYS2 下载很慢或失败

可以更换国内镜像源，编辑 `C:\msys64\etc\pacman.d\mirrorlist.mingw64`，在文件最前面添加：

```
Server = https://mirrors.tuna.tsinghua.edu.cn/msys2/mingw/mingw64/
Server = https://mirrors.ustc.edu.cn/msys2/mingw/mingw64/
```

然后执行：
```bash
pacman -Syu
```

### 4. SigFlow 提示找不到 Verilator

SigFlow 会自动检测以下位置的 Verilator：
- `C:\msys64\mingw64\bin\verilator.exe`
- PATH 环境变量中的 `verilator.exe`
- `VERILATOR_ROOT` 指定的路径

如果安装在非标准位置，请确保添加到 PATH 或设置 `VERILATOR_ROOT`。

---

## 下一步

安装完成后：

1. 重新编译 SigFlow 项目
2. 打开 Verilog 文件
3. 选择菜单 `Simulate -> Compile Simulation Model` (F5)
4. 输入顶层模块名
5. 等待编译完成

---

## 参考链接

- MSYS2 官网: https://www.msys2.org/
- Verilator 官网: https://verilator.org/
- Verilator GitHub: https://github.com/verilator/verilator
