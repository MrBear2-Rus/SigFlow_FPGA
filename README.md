# SigFlow FPGA

SigFlow FPGA 是带图形界面的 FPGA 开发工作区，包含 RTL 编辑、仿真、综合、布局布线、打包、烧录与波形调试。核心服务和工具链插件位于 `core/`、`InnerPlugin/`；主程序位于 `main/`。

## 构建

当前受支持的构建方式是 **CMake + GCC**：Windows 使用 MinGW-w64，Linux 使用系统 GCC。项目的 CMake 配置会拒绝非 GCC 编译器；旧的 Visual Studio/MSVC 工程不作为当前构建入口。

请先按 [完整构建指南](docs/BUILD.md) 准备依赖包和 wxWidgets，再运行对应平台的配置与构建命令。Windows 的基本流程如下（路径请替换为本机实际位置）：

```powershell
Expand-Archive -Force 3rd.zip .
Expand-Archive -Force external.zip .
Expand-Archive -Force tools.zip .

$mingwRoot = "E:/path/to/mingw64"
$wxRoot = "E:/path/to/repo/3rd/wx-mingw"
cmake -S . -B build-gcc -G "MinGW Makefiles" `
  -DCMAKE_TOOLCHAIN_FILE="cmake/toolchains/mingw-gcc.cmake" `
  -DMINGW_ROOT="$mingwRoot" `
  -DSIGFLOW_USE_LOCAL_WX=OFF `
  -DSIGFLOW_WX_ROOT="$wxRoot" `
  -DSIGFLOW_WX_CONFIG_DIR="$wxRoot/debug/lib/mswud" `
  -DSIGFLOW_WX_LIB_DIR="$wxRoot/debug/lib"
cmake --build build-gcc --target sigflow --parallel
```

Windows 构建还需要项目负责人分发的 MinGW 版 wxWidgets；解压位置及其他目录布局见构建指南。Linux、离线构建、运行与测试命令也以该指南为准。

当前构建不再编译或链接 slang，旧的 `SIGFLOW_SLANG_ROOT` 配置不再需要。
