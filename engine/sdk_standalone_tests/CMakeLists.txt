cmake_minimum_required(VERSION 3.12)
project(vision_sdk_consumer LANGUAGES C CXX)
if(NOT IS_DIRECTORY "${VISION_SDK_SOURCE_DIR}")
    message(FATAL_ERROR "Set VISION_SDK_SOURCE_DIR to a directory containing C/C++ sources")
endif()
get_filename_component(VISION_SDK_SOURCE_DIR "${VISION_SDK_SOURCE_DIR}" REALPATH)
if(VISION_SDK_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    message(FATAL_ERROR "Select one test directory, not the standalone tests collection")
endif()
if(VISION_SDK_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
    message(FATAL_ERROR "Use a separate build directory")
endif()
get_filename_component(sdk_output_name "${VISION_SDK_SOURCE_DIR}" NAME)
set(sdk_output_path "${VISION_SDK_SOURCE_DIR}/${sdk_output_name}")
# The executable now sits beside user sources. Never replace a source file,
# directory or symlink that happens to have the same name as the test directory.
if(IS_DIRECTORY "${sdk_output_path}" OR IS_SYMLINK "${sdk_output_path}")
    message(FATAL_ERROR "Executable path is already a directory or symlink: ${sdk_output_path}")
elseif(EXISTS "${sdk_output_path}")
    file(READ "${sdk_output_path}" sdk_output_magic LIMIT 4 HEX)
    if(NOT sdk_output_magic STREQUAL "7f454c46")
        message(FATAL_ERROR "Refusing to overwrite a non-ELF file: ${sdk_output_path}")
    endif()
endif()

# CONFIGURE_DEPENDS also discovers added/removed sources on direct CMake builds.
# Filter relative paths so a test under a parent named 'build' still works.
file(GLOB_RECURSE sdk_candidates CONFIGURE_DEPENDS LIST_DIRECTORIES false
    RELATIVE "${VISION_SDK_SOURCE_DIR}"
    "${VISION_SDK_SOURCE_DIR}/*.c" "${VISION_SDK_SOURCE_DIR}/*.C"
    "${VISION_SDK_SOURCE_DIR}/*.cc" "${VISION_SDK_SOURCE_DIR}/*.cpp"
    "${VISION_SDK_SOURCE_DIR}/*.cxx"
    "${VISION_SDK_SOURCE_DIR}/*.h" "${VISION_SDK_SOURCE_DIR}/*.hh"
    "${VISION_SDK_SOURCE_DIR}/*.hpp" "${VISION_SDK_SOURCE_DIR}/*.hxx")
set(sdk_sources)
set(sdk_headers)
file(RELATIVE_PATH sdk_build_relative "${VISION_SDK_SOURCE_DIR}" "${CMAKE_BINARY_DIR}")
foreach(relative IN LISTS sdk_candidates)
    if(relative STREQUAL sdk_output_name)
        continue()
    endif()
    if(relative MATCHES "(^|/)(build|cmake-build-[^/]*|CMakeFiles|\\.[^/]+)(/|$)")
        continue()
    endif()
    string(FIND "${relative}" "${sdk_build_relative}/" sdk_inside_build)
    if(sdk_inside_build EQUAL 0)
        continue()
    endif()
    if(relative MATCHES "\\.(c|C|cc|cpp|cxx)$")
        list(APPEND sdk_sources "${VISION_SDK_SOURCE_DIR}/${relative}")
    else()
        list(APPEND sdk_headers "${VISION_SDK_SOURCE_DIR}/${relative}")
    endif()
endforeach()
if(NOT sdk_sources)
    message(FATAL_ERROR "No .c/.cc/.cpp/.cxx/.C source files found in ${VISION_SDK_SOURCE_DIR}")
endif()

set(VISION_BUILD_APPLICATION OFF CACHE BOOL "Build business program" FORCE)
set(VISION_INFERENCE_ONLY OFF CACHE BOOL "Only inference" FORCE)
add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/.." engine EXCLUDE_FROM_ALL)
add_executable(sdk_example ${sdk_sources} ${sdk_headers})
target_include_directories(sdk_example PRIVATE
    "${VISION_SDK_SOURCE_DIR}" "${VISION_SDK_SOURCE_DIR}/include")
# Runtime exposes the complete public SDK and its transitive libraries. Consumers
# only include the headers they use; linking is shared by all test folders.
target_link_libraries(sdk_example PRIVATE Vision::Runtime)
# Keep assertions in the developer test; SDK libraries still use Release optimization.
target_compile_options(sdk_example PRIVATE -UNDEBUG)
set_target_properties(sdk_example PROPERTIES
    LINKER_LANGUAGE CXX
    RUNTIME_OUTPUT_DIRECTORY "${VISION_SDK_SOURCE_DIR}"
    OUTPUT_NAME "${sdk_output_name}")
list(LENGTH sdk_sources sdk_source_count)
message(STATUS "Standalone test: ${sdk_output_name} (${sdk_source_count} source files, public SDK)")
