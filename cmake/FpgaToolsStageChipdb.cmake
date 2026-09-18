# =============================================================================
# FpgaToolsStageChipdb.cmake — 把 nextpnr 生成的 chipdb 搬到主程序查找的位置
#
# 由 cmake/FpgaTools.cmake 以 `cmake -P` 方式调用。
# nextpnr 各版本把 himbaechel/gowin 的 chipdb 放在不同目录，而 SigFlow 的
# NextpnrExecutor::ValidateNextpnrRuntime() 固定查找：
#     <nextpnr>/share/himbaechel/gowin/chipdb-GW1N-9C.bin
# 因此构建完成后统一收拢一次；已经存在则不覆盖（例如仓库里预置的 13MB chipdb）。
#
# 入参： src = nextpnr 构建目录   dst = 落位后的 share/himbaechel/gowin
# =============================================================================

if(NOT DEFINED dst OR (NOT DEFINED src1 AND NOT DEFINED src))
    message(FATAL_ERROR "FpgaToolsStageChipdb: 需要 -Dsrc1=<构建目录> [-Dsrc2=<安装前缀>] -Ddst=<目标目录>")
endif()

# 搜索根：构建目录 + 安装前缀（分开传参，避免分号/引号被写进变量值）
set(_roots "")
if(DEFINED src1)
    list(APPEND _roots "${src1}")
endif()
if(DEFINED src2)
    list(APPEND _roots "${src2}")
endif()
if(DEFINED src)   # 兼容旧写法
    list(APPEND _roots ${src})
endif()

set(_all_chipdbs "")
foreach(_root IN LISTS _roots)
    if(EXISTS "${_root}")
        file(GLOB_RECURSE _found "${_root}/chipdb-*.bin")
        list(APPEND _all_chipdbs ${_found})
    endif()
endforeach()
list(REMOVE_DUPLICATES _all_chipdbs)
set(_chipdbs ${_all_chipdbs})
list(LENGTH _chipdbs _count)
message(STATUS "[FPGA tools] 搜索根: ${_roots}")

foreach(_f IN LISTS _chipdbs)
    get_filename_component(_name "${_f}" NAME)
    set(_target "${dst}/${_name}")
    if(EXISTS "${_target}")
        message(STATUS "[FPGA tools] 已存在，跳过: ${_target}")
    else()
        file(COPY "${_f}" DESTINATION "${dst}")
        message(STATUS "[FPGA tools] 已落位: ${_target}")
    endif()
endforeach()

# 仓库里预置的那份也保留（若构建没产出 GW1N-9C，至少还有兜底）
if(NOT EXISTS "${dst}/chipdb-GW1N-9C.bin")
    message(WARNING
        "[FPGA tools] 未生成 chipdb-GW1N-9C.bin。"
        "请确认 nextpnr 构建启用了 -DHIMBAECHEL_UARCH=gowin 且能导入 apycula。")
endif()
