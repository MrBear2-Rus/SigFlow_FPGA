# =============================================================================
# FpgaToolsNextpnrBoost185Patch.cmake — nextpnr-0.7 与 Boost 1.85 的兼容补丁
#
# Boost 1.85 移除了废弃头 boost/filesystem/convenience.hpp，而 nextpnr 0.7 的
#   bba/main.cc
#   common/kernel/command.cc
# 还在包含它。这两处实际只用了 boost::filesystem::path(...).stem()，
# 它在核心头 path.hpp 里 —— 直接替换 include 即可。
#
# 用法（脚本模式）：
#   cmake -DSRC=<nextpnr 源码根> -P FpgaToolsNextpnrBoost185Patch.cmake
# 幂等：已替换过（不含旧 include）时静默返回。
# =============================================================================
set(_root "${SRC}")
if(NOT _root)
    set(_root "${CMAKE_CURRENT_SOURCE_DIR}")
endif()

set(_files "bba/main.cc" "common/kernel/command.cc")
foreach(_f IN LISTS _files)
    if(NOT EXISTS "${_root}/${_f}")
        continue()
    endif()
    file(READ "${_root}/${_f}" _content)
    if(_content MATCHES "boost/filesystem/convenience\\.hpp")
        string(REPLACE "#include <boost/filesystem/convenience.hpp>"
                       "#include <boost/filesystem/path.hpp>"
                       _content "${_content}")
        file(WRITE "${_root}/${_f}" "${_content}")
        message(STATUS "[nextpnr patch] ${_f}: convenience.hpp -> path.hpp (Boost>=1.85)")
    endif()
endforeach()
