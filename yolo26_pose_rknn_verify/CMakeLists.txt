cmake_minimum_required(VERSION 3.16)

project(yolo26_pose_rknn_verify LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(OpenCV REQUIRED COMPONENTS core imgproc imgcodecs videoio highgui)
find_package(Threads REQUIRED)

set(RKNN_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../vision_analysis/vendor/rknn/2.4.2a2")
set(RKNN_INCLUDE_DIR "${RKNN_SDK_DIR}/include")
set(RKNN_LIBRARY "${RKNN_SDK_DIR}/lib/aarch64/librknnrt.so")

if(NOT EXISTS "${RKNN_INCLUDE_DIR}/rknn_api.h")
    message(FATAL_ERROR "RKNN header not found: ${RKNN_INCLUDE_DIR}/rknn_api.h")
endif()
if(NOT EXISTS "${RKNN_LIBRARY}")
    message(FATAL_ERROR "RKNN runtime not found: ${RKNN_LIBRARY}")
endif()

add_library(rknnrt_vendor SHARED IMPORTED GLOBAL)
set_target_properties(rknnrt_vendor PROPERTIES IMPORTED_LOCATION "${RKNN_LIBRARY}")

add_executable(yolo26_pose_verify main.cpp)
target_include_directories(yolo26_pose_verify PRIVATE "${RKNN_INCLUDE_DIR}")
target_link_libraries(yolo26_pose_verify PRIVATE rknnrt_vendor ${OpenCV_LIBS} Threads::Threads dl)
target_compile_options(yolo26_pose_verify PRIVATE -Wall -Wextra -Wpedantic)
set_target_properties(yolo26_pose_verify PROPERTIES
    BUILD_RPATH "$ORIGIN/lib"
    INSTALL_RPATH "$ORIGIN/lib")

add_custom_command(TARGET yolo26_pose_verify POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:yolo26_pose_verify>/lib"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${RKNN_LIBRARY}"
            "$<TARGET_FILE_DIR:yolo26_pose_verify>/lib/librknnrt.so"
    COMMENT "Copying the matching RKNN runtime beside the executable")

