# =============================================================================
# StageRuntimeLibs.cmake — 把 sigflow 的"系统侧"共享库依赖整体落位到 RUNPATH 目录
#
# 背景（为什么需要这份"拷贝"）：
#   sigflow 在构建机上链接的库来自系统路径（/usr/lib、/lib），运行期依赖
#   运行环境的系统库视图。一旦运行环境看不到某个库——典型是 openKylin 的
#   KARE（开明）沙箱终端，其 /usr 是独立快照，缺少宿主机上不太常用的包
#   （libpcre2-32：wxWidgets Unicode 正则；libftdi1：FTDI 直连）——启动即报
#       "error while loading shared libraries: libXXX.so.N: cannot open
#        shared object file"
#   逐个库手工救火不可维护，本脚本对**最终二进制**做一次 ldd，把所有
#   非基础、非项目内的依赖全部落位，使其自洽。
#
# 判定规则（三者都满足才落位）：
#   1. 不是基础运行库（libc/libm/libstdc++/ld-linux 等——必须用运行环境
#      自身的副本，拷贝反而遮蔽正确版本）；
#   2. 解析路径在项目树**外**（自建的 wxWidgets、FpgaTools 自建的 libftdi
#      都在项目内，本来就随构建分发，无需搬）；
#   3. 落位目录里还没有同名库（幂等，copy_if_different 增量更新）。
#
# 已验证：KARE 沙箱与宿主的 libc/libgtk-3 逐字节一致（同一系统快照），
# 落位宿主侧库进去不存在版本冲突；沙箱缺的只是个别包。
#
# 用法（cmake -P 脚本模式，通常由 sigflow 的 POST_BUILD 调用）：
#   cmake -DBIN=<sigflow 可执行文件> -DDST=<落位 lib 目录> -DROOT=<项目根> \
#         -P StageRuntimeLibs.cmake
# =============================================================================

if(NOT DEFINED BIN OR NOT DEFINED DST OR NOT DEFINED ROOT)
    message(FATAL_ERROR
        "用法: cmake -DBIN=<二进制> -DDST=<落位目录> -DROOT=<项目根> -P StageRuntimeLibs.cmake")
endif()

find_program(LDD_EXE NAMES ldd)
if(NOT LDD_EXE)
    # 非 Linux / 精简工具链：无 ldd 可用，静默跳过
    message(STATUS "StageRuntimeLibs: 无 ldd，跳过运行库落位")
    return()
endif()

execute_process(
    COMMAND "${LDD_EXE}" "${BIN}"
    OUTPUT_VARIABLE _ldd
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
string(REGEX REPLACE "\r?\n" ";" _lines "${_ldd}")

# 基础运行库：必须使用运行环境自身的副本，绝不落位
set(_base_libs
    "libc.so" "libm.so" "libpthread.so" "libdl.so" "librt.so"
    "libresolv.so" "libgcc_s.so" "libstdc++.so" "ld-linux" "linux-vdso"
    "libutil.so" "libanl.so" "libBrokenLocale.so" "libcrypt.so" "libnss_")

file(MAKE_DIRECTORY "${DST}")
set(_staged 0)

foreach(_line IN LISTS _lines)
    # 形如：libpcre2-32.so.0 => /lib/x86_64-linux-gnu/libpcre2-32.so.0 (0x...)
    # （行首是制表符，不能加 ^ 锚定；linux-vdso/ld-linux 行没有 "=>"，自动被过滤）
    string(REGEX MATCH "([A-Za-z0-9._+-]+\\.so[0-9.]*) +=> +([^ ]+)" _m "${_line}")
    if(NOT CMAKE_MATCH_2)
        continue()
    endif()
    set(_name "${CMAKE_MATCH_1}")
    set(_path "${CMAKE_MATCH_2}")

    set(_skip FALSE)
    foreach(_b IN LISTS _base_libs)
        # 前缀匹配（不用正则：libstdc++.so 里的 '+' 是正则元字符）
        string(FIND "${_name}" "${_b}" _pos)
        if(_pos EQUAL 0)
            set(_skip TRUE)
            break()
        endif()
    endforeach()
    if(_skip)
        continue()
    endif()

    # 解析符号链接（/lib -> /usr/lib、libXXX.so.N -> libXXX.so.N.M.K）
    file(REAL_PATH "${_path}" _real)

    # 项目树内的库（自建 wx / 自建 libftdi）：随构建分发，无需搬
    string(FIND "${_real}" "${ROOT}" _pos)
    if(_pos EQUAL 0)
        continue()
    endif()

    # 目标文件名必须等于 SONAME（NEEDED 记录的名字；动态链接器按它查找）
    set(_soname "${_name}")
    find_program(READELF_EXE NAMES readelf)
    if(READELF_EXE)
        execute_process(
            COMMAND "${READELF_EXE}" -d "${_real}"
            OUTPUT_VARIABLE _dyn
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET)
        string(REGEX MATCH "SONAME[^:]*: *\\[([A-Za-z0-9._+-]+)\\]" _sm "${_dyn}")
        if(CMAKE_MATCH_1)
            set(_soname "${CMAKE_MATCH_1}")
        endif()
    endif()

    if(EXISTS "${DST}/${_soname}")
        continue()
    endif()

    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${_real}" "${DST}/${_soname}")
    message(STATUS "StageRuntimeLibs: ${_soname} -> ${DST}")
    math(EXPR _staged "${_staged} + 1")
endforeach()

if(_staged EQUAL 0)
    message(STATUS "StageRuntimeLibs: 全部依赖已就绪（${DST}）")
endif()
