cmake_minimum_required(VERSION 3.12)

# 导出 compile_commands.json：让 VS Code / clangd 的 IntelliSense 用与编译器完全相同的 -I（含 OpenCV），
# 从根上消除 cv:: 的红波浪线，且永远和真实编译保持一致（不用再往 third_party 拷 opencv2）。
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

cmake_host_system_information(RESULT arch_value QUERY OS_PLATFORM)
message(STATUS "architecture: " ${arch_value})

# ===================== 交叉编译工具链（通用配置）=====================
# 仅在非 ARM 主机上需要交叉编译工具链。
# 查找优先级（由高到低）：
#   1. 命令行 -DCROSS_CMAKE_FILE=<path>
#   2. 环境变量 CROSS_CMAKE_FILE
# 如需在正点原子 / Radxa / PINE64 等板子上直接板端编译，无需任何工具链文件。
if(NOT CMAKE_TOOLCHAIN_FILE AND (NOT "${arch_value}" STREQUAL "armv7l") AND (NOT "${arch_value}" STREQUAL "aarch64"))
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

project(vision_analysis LANGUAGES C CXX)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()
add_definitions(-Wall -Wpointer-arith -O3 -ffast-math)
file(STRINGS "${PROJECT_SOURCE_DIR}/../VERSION" RKVISION_VERSION LIMIT_COUNT 1)
set(RKVISION_ABI 1)
# Reject SDK layout/compiler mismatches when importing or replacing a runtime.
file(GLOB_RECURSE sdk_headers CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/include/rkvision/*.h")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${sdk_headers} "${PROJECT_SOURCE_DIR}/../VERSION")
set(abi_inputs "${RKVISION_ABI};${CMAKE_CXX_COMPILER_ID};${CMAKE_CXX_COMPILER_VERSION};${CMAKE_SYSTEM_PROCESSOR};${CMAKE_CXX_FLAGS};${CMAKE_CXX_STANDARD}")
foreach(header IN LISTS sdk_headers)
    file(SHA256 "${header}" header_hash)
    string(APPEND abi_inputs ";${header_hash}")
endforeach()

# ======================== pinned RKNN Runtime =========================
# RKNN 的头文件和用户态 Runtime 随源码锁定，避免不同构建机上的
# /usr/include、ldconfig 或 sysroot 顺序生成内容不同的发布包。
set(RKNN_VENDOR_VERSION "2.4.2a2")
set(RKNN_VENDOR_ROOT
    "${PROJECT_SOURCE_DIR}/vendor/rknn/${RKNN_VENDOR_VERSION}"
    CACHE PATH "Pinned RKNN Runtime SDK root"
)
set(RKNN_API_INCLUDE_DIR "${RKNN_VENDOR_ROOT}/include")
set(RKNN_API_HEADER "${RKNN_API_INCLUDE_DIR}/rknn_api.h")
set(RKNN_RT_LIBRARY "${RKNN_VENDOR_ROOT}/lib/aarch64/librknnrt.so")
set(RKNN_API_HEADER_SHA256
    "61eedea2dd0fed8e4ee667fef571bf6b07fc87495bb6282d65055f60c61e7079")
set(RKNN_RT_LIBRARY_SHA256
    "bf50d51705ae433013927a13520ae781b534fdb1481c47bdddbc726f63ed4970")

foreach(required_file IN ITEMS "${RKNN_API_HEADER}" "${RKNN_RT_LIBRARY}")
    if(NOT EXISTS "${required_file}")
        message(FATAL_ERROR
            "缺少项目锁定的 RKNN SDK 文件: ${required_file}\n"
            "请恢复 vendor/rknn/${RKNN_VENDOR_VERSION}，禁止回退到系统 RKNN Runtime。")
    endif()
endforeach()

file(SHA256 "${RKNN_API_HEADER}" RKNN_API_HEADER_ACTUAL_SHA256)
if(NOT RKNN_API_HEADER_ACTUAL_SHA256 STREQUAL RKNN_API_HEADER_SHA256)
    message(FATAL_ERROR
        "rknn_api.h 校验失败。\n"
        "期望: ${RKNN_API_HEADER_SHA256}\n"
        "实际: ${RKNN_API_HEADER_ACTUAL_SHA256}")
endif()

file(SHA256 "${RKNN_RT_LIBRARY}" RKNN_RT_LIBRARY_ACTUAL_SHA256)
if(NOT RKNN_RT_LIBRARY_ACTUAL_SHA256 STREQUAL RKNN_RT_LIBRARY_SHA256)
    message(FATAL_ERROR
        "librknnrt.so 校验失败。\n"
        "期望: ${RKNN_RT_LIBRARY_SHA256}\n"
        "实际: ${RKNN_RT_LIBRARY_ACTUAL_SHA256}")
endif()

add_library(rknnrt_vendor SHARED IMPORTED GLOBAL)
set_target_properties(rknnrt_vendor PROPERTIES
    IMPORTED_LOCATION "${RKNN_RT_LIBRARY}"
    IMPORTED_SONAME "librknnrt.so"
    INTERFACE_INCLUDE_DIRECTORIES "${RKNN_API_INCLUDE_DIR}"
)
message(STATUS
    "Pinned RKNN Runtime: ${RKNN_VENDOR_VERSION} (${RKNN_RT_LIBRARY_SHA256})")
# ======================== pinned RKNN Runtime END =====================

# ======================== third_party helpers =========================
set(toolkit_root ${CMAKE_CURRENT_SOURCE_DIR}/src/third_party)
set(common_root  ${toolkit_root})

if(EXISTS ${common_root}/gst_opt/api.cmake)
    include(${common_root}/gst_opt/api.cmake)
else()
    set(GSTOPT_INCLUDE_DIRS "")
    set(GSTOPT_SOURCE_DIRS "")
    set(GSTOPT_LIBS "")
endif()

set(api_inc
    ${GSTOPT_INCLUDE_DIRS}
)

# GStreamer 辅助实现直接随引擎源码编译。
file(GLOB api_srcs
    ${GSTOPT_SOURCE_DIRS}
)

set(sysLib_list
    ${GSTOPT_LIBS}
)
# ======================== third_party helpers END =========================

# ========================== custom ============================
find_package(OpenCV REQUIRED)
find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)

# 画面文字统一用 OpenCV freetype 模块渲染(中英文), 不再回退 Hershey。该模块为【必需】:
# 找不到 opencv2/freetype.hpp 直接报错, 而不是静默降级。opencv_freetype 库已含在 OpenCV_LIBS 中。
find_path(OPENCV_FREETYPE_HEADER opencv2/freetype.hpp PATHS ${OpenCV_INCLUDE_DIRS})
if(NOT OPENCV_FREETYPE_HEADER)
    message(FATAL_ERROR
        "未找到 OpenCV freetype 模块(opencv2/freetype.hpp)。\n"
        "本工程画面文字统一用 freetype 渲染(中文+英文), 该模块为必需。\n"
        "请安装带 contrib freetype 的 OpenCV(libopencv-contrib / 自编 opencv_contrib)。")
endif()
message(STATUS "OpenCV freetype module found -> overlay text rendered via FreeType (CN + EN)")

pkg_check_modules(GTK3 REQUIRED gtk+-3.0)

set(custom_inc
    ${OpenCV_INCLUDE_DIRS}
    ${GTK3_INCLUDE_DIRS}
    ${PROJECT_SOURCE_DIR}/src
    ${PROJECT_SOURCE_DIR}/include
)

set(custom_libs
    rga
    rknnrt_vendor
    ${OpenCV_LIBS}
    ${GTK3_LIBRARIES}
    Threads::Threads
    gpiod
)

aux_source_directory(${PROJECT_SOURCE_DIR}/src                          dir_srcs)
aux_source_directory(${PROJECT_SOURCE_DIR}/src/config                   dir_srcs)
aux_source_directory(${PROJECT_SOURCE_DIR}/src/runtime                  dir_srcs)
aux_source_directory(${PROJECT_SOURCE_DIR}/src/capturer                 dir_srcs)
aux_source_directory(${PROJECT_SOURCE_DIR}/src/pipeline                 dir_srcs)
aux_source_directory(${PROJECT_SOURCE_DIR}/src/inference                dir_srcs)
aux_source_directory(${PROJECT_SOURCE_DIR}/src/tracking                 dir_srcs)
aux_source_directory(${PROJECT_SOURCE_DIR}/src/control                  dir_srcs)
aux_source_directory(${PROJECT_SOURCE_DIR}/src/gpio                     dir_srcs)
file(GLOB_RECURSE core_logic_srcs CONFIGURE_DEPENDS
    "${PROJECT_SOURCE_DIR}/src/logic/core/*.cpp"
    "${PROJECT_SOURCE_DIR}/src/logic/core/*.cc"
    "${PROJECT_SOURCE_DIR}/src/logic/core/*.cxx")
list(APPEND dir_srcs ${core_logic_srcs})
aux_source_directory(${PROJECT_SOURCE_DIR}/src/display                  dir_srcs)
aux_source_directory(${PROJECT_SOURCE_DIR}/src/rtsp                     dir_srcs)
aux_source_directory(${PROJECT_SOURCE_DIR}/src/yolo                     dir_srcs)
aux_source_directory(${PROJECT_SOURCE_DIR}/src/recorder                 dir_srcs)
aux_source_directory(${PROJECT_SOURCE_DIR}/src/event                    dir_srcs)
set(cjson_src ${PROJECT_SOURCE_DIR}/src/third_party/json/cJSON.c)
# ========================== custom ============================

# Public SDK plus a shared engine. Direct project CMake builds the runtime locally;
# build.sh supplies RKVISION_RUNTIME_DIR to reuse one engine build across projects.
string(SHA256 RKVISION_ABI_TAG "${abi_inputs};${OpenCV_VERSION}")
set(runtime_metadata "${CMAKE_CURRENT_BINARY_DIR}/rkvision-runtime.cmake")
file(WRITE "${runtime_metadata}"
    "set(RKVISION_BUILT_ABI_TAG \"${RKVISION_ABI_TAG}\")\nset(RKVISION_BUILT_VERSION \"${RKVISION_VERSION}\")\nset(RKVISION_BUILT_TYPE \"${CMAKE_BUILD_TYPE}\")\n")
if(RKVISION_RUNTIME_DIR)
    get_filename_component(RKVISION_RUNTIME_DIR "${RKVISION_RUNTIME_DIR}" ABSOLUTE)
    include("${RKVISION_RUNTIME_DIR}/rkvision-runtime.cmake")
    if(NOT RKVISION_BUILT_ABI_TAG STREQUAL RKVISION_ABI_TAG OR
       NOT RKVISION_BUILT_VERSION STREQUAL RKVISION_VERSION OR
       NOT RKVISION_BUILT_TYPE STREQUAL CMAKE_BUILD_TYPE)
        message(FATAL_ERROR "Shared runtime SDK/compiler/version/build type mismatch; rebuild the engine")
    endif()
    add_library(rkvision SHARED IMPORTED GLOBAL)
    set_target_properties(rkvision PROPERTIES
        IMPORTED_LOCATION "${RKVISION_RUNTIME_DIR}/librkvision.so.${RKVISION_ABI}"
        IMPORTED_SONAME "librkvision.so.${RKVISION_ABI}")
    # Copy actual files (not symlinks) so build products and archives are relocatable.
    add_custom_target(rkvision_stage ALL
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_BINARY_DIR}/libs"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "$<TARGET_FILE:rkvision>" "${CMAKE_BINARY_DIR}/libs/librkvision.so.${RKVISION_ABI}"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${runtime_metadata}" "${CMAKE_BINARY_DIR}/libs/rkvision-runtime.cmake"
        VERBATIM)
else()
    add_library(rkvision SHARED ${dir_srcs} ${api_srcs} ${cjson_src})
    target_include_directories(rkvision PRIVATE ${custom_inc} ${api_inc})
    target_compile_definitions(rkvision PRIVATE
        RKVISION_ENGINE_VERSION="${RKVISION_VERSION}" RKVISION_BUILD_ABI_TAG="${RKVISION_ABI_TAG}")
    # Preserve direct internal function calls despite the shared-library boundary.
    target_compile_options(rkvision PRIVATE -fno-semantic-interposition)
    target_link_libraries(rkvision PRIVATE ${custom_libs} ${sysLib_list})
    set_target_properties(rkvision PROPERTIES
        VERSION "${RKVISION_VERSION}" SOVERSION "${RKVISION_ABI}"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/libs"
        LINK_FLAGS "-Wl,--no-undefined,-Bsymbolic-functions"
        BUILD_WITH_INSTALL_RPATH TRUE INSTALL_RPATH "$ORIGIN")
    file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/libs")
    configure_file("${runtime_metadata}" "${CMAKE_BINARY_DIR}/libs/rkvision-runtime.cmake" COPYONLY)
endif()
add_library(rkvision::runtime ALIAS rkvision)

if(NOT VISION_PROJECT_DIR)
    return() # Standalone engine build: cmake -S vision_analysis -B build/runtime/...
endif()
get_filename_component(VISION_PROJECT_DIR "${VISION_PROJECT_DIR}" ABSOLUTE)
set(VISION_LOGIC_ROOT "${VISION_PROJECT_DIR}/logic")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${VISION_PROJECT_DIR}/project.json" "${PROJECT_SOURCE_DIR}/../VERSION")
if(NOT EXISTS "${VISION_PROJECT_DIR}/project.json")
    message(FATAL_ERROR "Missing application project.json: ${VISION_PROJECT_DIR}")
endif()
find_program(PYTHON3_EXECUTABLE NAMES python3)
if(NOT PYTHON3_EXECUTABLE)
    message(FATAL_ERROR "Project validation requires python3")
endif()
execute_process(COMMAND "${PYTHON3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/../tools/project/vision.py"
    check "${VISION_PROJECT_DIR}" RESULT_VARIABLE project_check)
if(NOT project_check EQUAL 0)
    message(FATAL_ERROR "Application validation failed")
endif()
file(GLOB_RECURSE app_logic_srcs CONFIGURE_DEPENDS
    "${VISION_LOGIC_ROOT}/*.cpp" "${VISION_LOGIC_ROOT}/*.cc" "${VISION_LOGIC_ROOT}/*.cxx")
# Public SDK is the only engine include path visible to application compilation.
add_library(rkvision_sdk INTERFACE)
add_library(rkvision::sdk ALIAS rkvision_sdk)
target_include_directories(rkvision_sdk INTERFACE "${PROJECT_SOURCE_DIR}/include" ${OpenCV_INCLUDE_DIRS})
if(app_logic_srcs)
    add_library(vision_app OBJECT ${app_logic_srcs})
    target_link_libraries(vision_app PRIVATE rkvision::sdk)
    target_include_directories(vision_app PRIVATE "${VISION_PROJECT_DIR}")
    list(APPEND app_sources $<TARGET_OBJECTS:vision_app>)
endif()
file(GLOB_RECURSE logic_validation_files CONFIGURE_DEPENDS "${VISION_LOGIC_ROOT}/*")
# A changed file list invalidates the embedded catalog when a module is removed too.
# Depending only on surviving manifests would leave the old catalog up to date.
set(logic_input_list "${CMAKE_CURRENT_BINARY_DIR}/generated/logic_inputs.txt")
file(GENERATE OUTPUT "${logic_input_list}" CONTENT "${logic_validation_files}\n")
set(logic_catalog_cpp "${CMAKE_CURRENT_BINARY_DIR}/generated/logic_catalog_embedded.cpp")
add_custom_command(
    OUTPUT "${logic_catalog_cpp}"
    COMMAND "${PYTHON3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/scripts/generate_logics_catalog.py"
            --logic-root "${VISION_LOGIC_ROOT}"
            --cpp-output "${logic_catalog_cpp}"
    DEPENDS "${PROJECT_SOURCE_DIR}/scripts/generate_logics_catalog.py"
            "${PROJECT_SOURCE_DIR}/metadata/catalog.json" "${logic_input_list}" ${logic_validation_files}
    COMMENT "Validating and embedding application parameter schemas" VERBATIM)
list(APPEND app_sources "${logic_catalog_cpp}")

add_executable(vision_analysis "${PROJECT_SOURCE_DIR}/application/main.cpp" ${app_sources})
target_compile_definitions(vision_analysis PRIVATE RKVISION_BUILD_ABI_TAG="${RKVISION_ABI_TAG}")
target_link_libraries(vision_analysis PRIVATE rkvision::sdk rkvision::runtime ${OpenCV_LIBS})
if(TARGET rkvision_stage)
    add_dependencies(vision_analysis rkvision_stage)
endif()
set_target_properties(vision_analysis PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    BUILD_WITH_INSTALL_RPATH TRUE INSTALL_RPATH "$ORIGIN/libs")
