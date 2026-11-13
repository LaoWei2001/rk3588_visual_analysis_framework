cmake_host_system_information(RESULT arch_value QUERY OS_PLATFORM)
message(STATUS "architecture: " ${arch_value})

# ===================== 交叉编译工具链（通用配置）=====================
# 仅在非 ARM 主机上需要交叉编译工具链。
# 查找优先级（由高到低）：
#   1. 命令行 -DCROSS_CMAKE_FILE=<path>
#   2. 环境变量 CROSS_CMAKE_FILE
# 如需在正点原子 / Radxa / PINE64 等板子上直接板端编译，无需任何工具链文件。
if((NOT "${arch_value}" STREQUAL "armv7l") AND (NOT "${arch_value}" STREQUAL "aarch64"))
    if(DEFINED CROSS_CMAKE_FILE AND EXISTS "${CROSS_CMAKE_FILE}")
        message(STATUS "Cross-compile toolchain (via -DCROSS_CMAKE_FILE): ${CROSS_CMAKE_FILE}")
        include("${CROSS_CMAKE_FILE}")
    elseif(DEFINED ENV{CROSS_CMAKE_FILE} AND EXISTS "$ENV{CROSS_CMAKE_FILE}")
        message(STATUS "Cross-compile toolchain (via CROSS_CMAKE_FILE env): $ENV{CROSS_CMAKE_FILE}")
        include("$ENV{CROSS_CMAKE_FILE}")
    else()
        message(WARNING
            "Cross-compile toolchain file not found!\n"
            "Please provide one via:\n"
            "  cmake -DCROSS_CMAKE_FILE=/path/to/cross.cmake ..\n"
            "  export CROSS_CMAKE_FILE=/path/to/cross.cmake"
        )
    endif()
endif()

# ===================== 交叉编译工具链 END =====================

