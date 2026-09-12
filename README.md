# SigFlow CMake 构建副本

该目录是从 `SigFlow_FPGA` 提取的独立 CMake 构建工作区，不修改原仓库。

已包含 `tools/` 下的构建与验证脚本、Verilator，以及主程序运行时需要的 `external/fpga-tools/runtime/`。

当前目标是先在 Windows x64 上复现现有主程序。源码仍包含 Win32 API，Linux/麒麟构建需要后续完成平台抽象层后再启用。

GCC 入口已加入，但当前副本中的 wxWidgets 和 slang 仍是 MSVC 版本，不能直接给 GCC 链接。需要先准备 GCC 版 wxWidgets 和 slang SDK。

## 构建

在本目录打开“适用于 VS 的开发者 PowerShell”，执行：

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Debug --parallel
```

生成的程序位于 `build/Debug/sigflow.exe`。运行前会自动复制 wxWidgets、slang DLL 和资源文件；运行时工具链保留在工作区的 `external/fpga-tools/runtime/`。

如果本机的 CMake 不识别 `Visual Studio 17 2022`，先执行 `cmake --help` 查看已安装的生成器，并替换 `-G` 参数；本机验证使用的是 `Visual Studio 18 2026`。

也可以在 VS Code 中打开本目录，安装 CMake Tools 扩展后选择 `sigflow` 目标构建；VS Code 只是编辑器，CMake 负责生成和构建工程。

## GCC 构建

准备 GCC 版 wxWidgets 后，指定其生成的 `wx/setup.h` 目录和库目录；slang SDK 需要提供 GCC 可链接的 `libsvlang.a` 或 `libsvlang.dll.a`：

```powershell
cmake -S . -B build-gcc -G "MinGW Makefiles" `
  -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/mingw-gcc.cmake `
  -DMINGW_ROOT="E:/path/to/mingw64" `
  -DSIGFLOW_WX_ROOT="E:/path/to/wxwidgets-gcc" `
  -DSIGFLOW_WX_CONFIG_DIR="E:/path/to/wxwidgets-gcc/lib/gcc_x64_dll/mswud" `
  -DSIGFLOW_WX_LIB_DIR="E:/path/to/wxwidgets-gcc/lib/gcc_x64_dll" `
  -DSIGFLOW_SLANG_ROOT="E:/path/to/slang-gcc"
cmake --build build-gcc -- -j2
```
