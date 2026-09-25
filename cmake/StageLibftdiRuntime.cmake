# =============================================================================
# StageLibftdiRuntime.cmake — 把 libftdi 运行库自动落位到 RPATH 指向的目录
#
# 背景（为什么需要这份"拷贝"）：
#   sigflow 二进制的 BUILD_RPATH / INSTALL_RPATH 固定指向
#       external/fpga-tools/runtime/deps/libftdi/lib
#   当链接的 libftdi 实际来自系统路径（如 openKylin 的 /usr/lib/.../libftdi1.so）
#   而非该落位目录时，RPATH 目录里并没有 so，运行期就完全依赖系统库视图。
#   一旦运行环境的系统库视图不完整——典型是 openKylin 的 KARE（开明）沙箱
#   终端看不到宿主机新装的包——就会报
#       "error while loading shared libraries: libftdi1.so.2: cannot open
#        shared object file"
#   （Windows 上 DLL 按可执行文件目录查找，不适用本脚本。）
#
# 行为：
#   1. 库已在落位目录里（FpgaTools.cmake 的 sigflow_libftdi_build 自建场景）
#      → 直接返回，不做任何事；
#   2. 库来自系统 → 读取其 SONAME（动态链接器按 NEEDED 名查找，必须用
#      SONAME 命名落位文件），连同其非基础依赖（libusb-1.0 等）一并
#      copy_if_different 到落位目录。libc/libstdc++ 等基础运行库不搬，
#      避免遮蔽系统副本引发版本冲突。
#
# 用法（cmake -P 脚本模式）：
#   cmake -Dlib:FILEPATH=<链接用的库路径> -Ddst:PATH=<落位 lib 目录> \
#         -P StageLibftdiRuntime.cmake
# =============================================================================

if(NOT DEFINED lib OR NOT DEFINED dst)
    message(FATAL_ERROR
        "用法: cmake -Dlib=<库路径> -Ddst=<落位目录> -P StageLibftdiRuntime.cmake")
endif()

# 解析 dev 符号链接（libftdi1.so -> libftdi1.so.2.5.0），拿到真实文件
file(REAL_PATH "${lib}" lib_real)

# 库本就在落位目录里：自建场景，无需处理
cmake_path(SET dst_dir NORMALIZE "${dst}")
cmake_path(IS_PREFIX dst_dir "${lib_real}" NORMALIZE _lib_in_dst)
if(_lib_in_dst)
    return()
endif()

# readelf / ldd 缺失（如非 Linux、精简工具链）时静默跳过，不影响构建
find_program(READELF_EXE NAMES readelf)
find_program(LDD_EXE NAMES ldd)
if(NOT READELF_EXE AND NOT LDD_EXE)
    message(STATUS "StageLibftdiRuntime: 无 readelf/ldd，跳过 libftdi 运行库落位")
    return()
endif()

file(MAKE_DIRECTORY "${dst_dir}")

# ---------------------------------------------------------------------------
# 1) libftdi1 本体：目标文件名必须等于 SONAME（NEEDED 记录的名字）
# ---------------------------------------------------------------------------
set(soname "")
if(READELF_EXE)
    execute_process(
        COMMAND "${READELF_EXE}" -d "${lib_real}"
        OUTPUT_VARIABLE _dyn
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    string(REGEX MATCH "SONAME[^:]*: *\\[([A-Za-z0-9._+-]+)\\]" _m "${_dyn}")
    if(CMAKE_MATCH_1)
        set(soname "${CMAKE_MATCH_1}")
    endif()
endif()
if(NOT soname)
    # 兜底：用真实文件名（无 SONAME 的库按文件名被加载）
    get_filename_component(soname "${lib_real}" NAME)
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${lib_real}" "${dst_dir}/${soname}")
message(STATUS "StageLibftdiRuntime: ${soname} -> ${dst_dir}")

# ---------------------------------------------------------------------------
# 2) 非基础依赖（libusb-1.0 等）：从 ldd 输出解析，逐个落位
#    基础运行库（libc/libm/libstdc++/ld-linux...）不搬——沙箱里它们来自
#    基础镜像、必然可见，搬过去反而可能遮蔽系统的正确版本。
# ---------------------------------------------------------------------------
if(NOT LDD_EXE)
    return()
endif()
execute_process(
    COMMAND "${LDD_EXE}" "${lib_real}"
    OUTPUT_VARIABLE _deps
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
string(REGEX REPLACE "\r?\n" ";" _dep_lines "${_deps}")

set(_base_libs
    "libc.so" "libm.so" "libpthread.so" "libdl.so" "librt.so"
    "libresolv.so" "libgcc_s.so" "libstdc++.so" "ld-linux" "linux-vdso")

foreach(_line IN LISTS _dep_lines)
    # 形如：libusb-1.0.so.0 => /lib/x86_64-linux-gnu/libusb-1.0.so.0.4.0 (0x...)
    # （行首是制表符，不能加 ^ 锚定；linux-vdso/ld-linux 行没有 "=>"，自动被过滤）
    string(REGEX MATCH "([A-Za-z0-9._+-]+\\.so[0-9.]*) +=> +([^ ]+)" _m "${_line}")
    if(NOT CMAKE_MATCH_2)
        continue()
    endif()
    set(_dep_name "${CMAKE_MATCH_1}")
    set(_dep_path "${CMAKE_MATCH_2}")

    set(_skip FALSE)
    foreach(_b IN LISTS _base_libs)
        # 前缀匹配（不用正则：libstdc++.so 里的 '+' 是正则元字符）
        string(FIND "${_dep_name}" "${_b}" _pos)
        if(_pos EQUAL 0)
            set(_skip TRUE)
            break()
        endif()
    endforeach()
    if(_skip)
        continue()
    endif()

    # 已在落位目录（含符号链接）则不重复拷贝
    if(EXISTS "${dst_dir}/${_dep_name}")
        continue()
    endif()

    file(REAL_PATH "${_dep_path}" _dep_real)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${_dep_real}" "${dst_dir}/${_dep_name}")
    message(STATUS "StageLibftdiRuntime: ${_dep_name} -> ${dst_dir}")
endforeach()
