# 上层业务工作区与底层引擎源码分离。二次开发者升级引擎时只需迁移整个
# projects/ 目录；也可通过 -DVISION_PROJECTS_ROOT=/path/to/projects 使用外部工作区。
set(VISION_PROJECTS_ROOT
    "${PROJECT_SOURCE_DIR}/../projects"
    CACHE PATH "Business logic, assets and project-owned services root"
)
get_filename_component(VISION_PROJECTS_ROOT "${VISION_PROJECTS_ROOT}" ABSOLUTE)
if(NOT EXISTS "${VISION_PROJECTS_ROOT}/modules" OR
   NOT EXISTS "${VISION_PROJECTS_ROOT}/global_modules" OR
   NOT EXISTS "${VISION_PROJECTS_ROOT}/catalog.json")
    message(FATAL_ERROR
        "业务工作区不完整: ${VISION_PROJECTS_ROOT}\n"
        "必须包含 modules/、global_modules/ 和 catalog.json")
endif()
message(STATUS "Business projects root: ${VISION_PROJECTS_ROOT}")
get_filename_component(VISION_REPOSITORY_ROOT "${PROJECT_SOURCE_DIR}/.." ABSOLUTE)
set(LOGIC_CATALOG_GENERATOR
    "${VISION_REPOSITORY_ROOT}/tools/build/generate_logics_catalog.py")
if(NOT EXISTS "${LOGIC_CATALOG_GENERATOR}")
    message(FATAL_ERROR "缺少 Logic 清单生成器: ${LOGIC_CATALOG_GENERATOR}")
endif()


# Business code receives public headers and OpenCV only.
# No engine/src, RKNN, GStreamer or GTK include path is exported.
add_library(vision_sdk INTERFACE)
add_library(Vision::SDK ALIAS vision_sdk)
target_include_directories(vision_sdk INTERFACE
    ${PROJECT_SOURCE_DIR}/include
)
target_include_directories(vision_sdk SYSTEM INTERFACE ${OpenCV_INCLUDE_DIRS})
target_compile_features(vision_sdk INTERFACE cxx_std_11)


file(GLOB_RECURSE project_logic_srcs
    "${VISION_PROJECTS_ROOT}/modules/*.cpp"
    "${VISION_PROJECTS_ROOT}/modules/*.cc"
    "${VISION_PROJECTS_ROOT}/modules/*.cxx"
    "${VISION_PROJECTS_ROOT}/global_modules/*.cpp"
    "${VISION_PROJECTS_ROOT}/global_modules/*.cc"
    "${VISION_PROJECTS_ROOT}/global_modules/*.cxx"
)

# OBJECT keeps static registration constructors in the final executable.
# Business sources directly include the public SDK definitions.
if(project_logic_srcs)
    add_library(vision_business OBJECT ${project_logic_srcs})
    target_link_libraries(vision_business PRIVATE Vision::SDK)
endif()

# 模块 logic.json 是参数 Schema 的唯一真源。构建时将聚合后的能力清单嵌入二进制，
# 因此 --debug 直接运行也能完成参数默认值、类型和热重载策略校验，不依赖外部 logics.json。
find_program(PYTHON3_EXECUTABLE NAMES python3)
if(NOT PYTHON3_EXECUTABLE)
    message(FATAL_ERROR "生成逻辑参数能力需要 python3")
endif()
file(GLOB logic_manifest_files
    "${VISION_PROJECTS_ROOT}/modules/*/logic.json"
    "${VISION_PROJECTS_ROOT}/global_modules/*/logic.json"
)
file(GLOB_RECURSE logic_validation_files
    "${VISION_PROJECTS_ROOT}/modules/*.cpp"
    "${VISION_PROJECTS_ROOT}/modules/*.cc"
    "${VISION_PROJECTS_ROOT}/modules/*.cxx"
    "${VISION_PROJECTS_ROOT}/modules/*.h"
    "${VISION_PROJECTS_ROOT}/modules/*.hpp"
    "${VISION_PROJECTS_ROOT}/global_modules/*.cpp"
    "${VISION_PROJECTS_ROOT}/global_modules/*.cc"
    "${VISION_PROJECTS_ROOT}/global_modules/*.cxx"
    "${VISION_PROJECTS_ROOT}/global_modules/*.h"
    "${VISION_PROJECTS_ROOT}/global_modules/*.hpp"
)
set(logic_catalog_cpp "${CMAKE_CURRENT_BINARY_DIR}/generated/logic_catalog_embedded.cpp")
# 文件列表变化也要刷新清单，避免删除模块后仍沿用缓存的参数 Schema。
set(logic_catalog_inputs "${CMAKE_CURRENT_BINARY_DIR}/generated/logic_catalog_inputs.txt")
file(GENERATE OUTPUT "${logic_catalog_inputs}"
    CONTENT "${logic_manifest_files}\n${logic_validation_files}\n")
add_custom_command(
    OUTPUT "${logic_catalog_cpp}"
    COMMAND "${PYTHON3_EXECUTABLE}"
            "${LOGIC_CATALOG_GENERATOR}"
            --logic-root "${VISION_PROJECTS_ROOT}"
            --cpp-output "${logic_catalog_cpp}"
    DEPENDS
            "${logic_catalog_inputs}"
            "${LOGIC_CATALOG_GENERATOR}"
            "${VISION_PROJECTS_ROOT}/catalog.json"
            ${logic_manifest_files}
            ${logic_validation_files}
    COMMENT "Validating and embedding per-logic parameter schemas"
    VERBATIM
)
