cmake_minimum_required(VERSION 3.10)
project(logic_parameters_unit_test CXX C)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_program(PYTHON3_EXECUTABLE NAMES python3)
if(NOT PYTHON3_EXECUTABLE)
    message(FATAL_ERROR "python3 is required")
endif()

set(PROJECT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")
set(GENERATED_CPP "${CMAKE_CURRENT_BINARY_DIR}/logic_catalog_embedded.cpp")
file(GLOB LOGIC_MANIFESTS "${PROJECT_ROOT}/src/logic/modules/*/logic.json")
file(GLOB_RECURSE LOGIC_SOURCES
    "${PROJECT_ROOT}/src/logic/modules/*.cpp"
    "${PROJECT_ROOT}/src/logic/modules/*.cc"
    "${PROJECT_ROOT}/src/logic/modules/*.cxx"
    "${PROJECT_ROOT}/src/logic/modules/*.h"
    "${PROJECT_ROOT}/src/logic/modules/*.hpp"
)

add_custom_command(
    OUTPUT "${GENERATED_CPP}"
    COMMAND "${PYTHON3_EXECUTABLE}"
            "${PROJECT_ROOT}/scripts/generate_logics_catalog.py"
            --logic-root "${PROJECT_ROOT}/src/logic"
            --cpp-output "${GENERATED_CPP}"
    DEPENDS
            "${PROJECT_ROOT}/scripts/generate_logics_catalog.py"
            "${PROJECT_ROOT}/src/logic/catalog.json"
            ${LOGIC_MANIFESTS}
            ${LOGIC_SOURCES}
    VERBATIM
)

add_executable(logic_parameters_unit_test
    logic_parameters_unit_test.cpp
    "${PROJECT_ROOT}/src/logic/core/logic_parameters.cpp"
    "${PROJECT_ROOT}/src/third_party/json/cJSON.c"
    "${GENERATED_CPP}"
)

target_include_directories(logic_parameters_unit_test PRIVATE
    "${PROJECT_ROOT}/src"
    "${PROJECT_ROOT}/src/third_party/json"
)
