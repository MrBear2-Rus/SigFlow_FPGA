# Verilator Windows 安装指南 (MSYS2 方法)

由于 vcpkg 不提供 Verilator，我们需要使用 MSYS2 来安装。

---

## 步骤 1：下载并安装 MSYS2

1. 访问官网下载安装程序：
   https://www.msys2.org/

2. 下载 `msys2-x86_64-latest.exe`

3. 运行安装程序，按提示安装（建议安装到 `C:\msys64`）

---

## 步骤 2：更新 MSYS2

安装完成后，打开 **MSYS2 MSYS** 终端（从开始菜单找到），执行：

```bash
# 更新包数据库和核心系统包
pacman -Syu

# 如果提示关闭终端，请关闭后重新打开，再次执行：
pacman -Su
```

---

## 步骤 3：安装 Verilator

在 MSYS2 终端中执行：

```bash
# 安装 Verilator
pacman -S mingw-w64-x86_64-verilator

# 同时安装一些常用工具（可选但推荐）
pacman -S mingw-w64-x86_64-gcc
pacman -S mingw-w64-x86_64-make
```

---

## 步骤 4：配置环境变量

### 添加到系统 PATH：

按 `Win + R`，输入 `sysdm.cpl`，回车，然后：n
高级 → 环境变量 → 系统变量 → 找到 Path → 编辑 → 新建

添加以下路径（根据你的 MSYS2 安装位置调整）：

```
C:\msys64\mingw64\bin
C:\msys64\usr\bin
```

### 添加 VERILATOR_ROOT：

在系统变量中点击 **新建**：
- 变量名：`VERILATOR_ROOT`
- 变量值：`C:\msys64\mingw64\share\verilator`

---

## 步骤 5：验证安装

**关闭所有命令行窗口**，然后重新打开 cmd 或 PowerShell：

```cmd
verilator --version
```

应该显示类似：
```
Verilator 5.006 2023-... rev ...
```

---

## 步骤 6：测试编译（可选）

创建一个测试文件 `test.v`：

```verilog
module test;
    initial begin
        $display("Hello from Verilator!");
        $finish;
    end
endmodule
```

在 cmd 中执行：

```cmd
verilator --cc test.v
```

如果没有报错，说明安装成功！

---

## 常见问题

### 1. "verilator 不是内部或外部命令"

- 确保已添加到 PATH
- 确保使用的是 **mingw64** 路径，不是 msys 路径
- **重新打开**命令行窗口

### 2. 找不到头文件

确保设置了 `VERILATOR_ROOT` 环境变量。

### 3. MSYS2 下载很慢

可以更换国内镜像源，编辑 `C:\msys64\etc\pacman.d\mirrorlist.mingw64`，在文件最前面添加：

```
Server = https://mirrors.tuna.tsinghua.edu.cn/msys2/mingw/mingw64/
Server = https://mirrors.ustc.edu.cn/msys2/mingw/mingw64/
```

然后执行 `pacman -Syu` 更新

---

## 下一步

安装完成后，回到 SigFlow 项目，应该可以直接使用仿真功能了！
