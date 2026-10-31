option(BUILD_DEPTH_TESTS "Build depth SDK and optional RK3588 hardware tests" OFF)
if(BUILD_DEPTH_TESTS)
    enable_testing()
    add_executable(depth_result_tests
        tests/inference/test_depth_results.cpp
        src/inference/inference_result_mapping.cpp
        src/inference/inference_roi_filter.cpp
        src/logic/core/channel_logic.cpp)
    target_include_directories(depth_result_tests PRIVATE ${custom_inc} ${api_inc})
    target_compile_options(depth_result_tests PRIVATE -UNDEBUG -ffunction-sections -fdata-sections)
    set_target_properties(depth_result_tests PROPERTIES LINK_FLAGS "-Wl,--gc-sections")
    target_link_libraries(depth_result_tests PRIVATE ${OpenCV_LIBS} Threads::Threads)
    add_test(NAME depth_result_regression COMMAND depth_result_tests)

    add_executable(depth_runtime_tests tests/inference/test_depth_runtime.cpp src/yolo/yolo26depth.cpp)
    target_include_directories(depth_runtime_tests PRIVATE ${custom_inc})
    target_compile_options(depth_runtime_tests PRIVATE -UNDEBUG)
    target_link_libraries(depth_runtime_tests PRIVATE ${OpenCV_LIBS} rknnrt_vendor rga Threads::Threads)
    set(DEPTH_TEST_MODEL "${VISION_REPOSITORY_ROOT}/yolo26n-depth-float.rknn" CACHE FILEPATH "Depth RKNN for hardware tests")
    set(DEPTH_TEST_IMAGE "${VISION_REPOSITORY_ROOT}/depth/assets/bus.jpg" CACHE FILEPATH "Depth test image")
    if(EXISTS "${DEPTH_TEST_MODEL}" AND EXISTS "${DEPTH_TEST_IMAGE}")
        add_test(NAME depth_runtime_regression COMMAND depth_runtime_tests "${DEPTH_TEST_MODEL}" "${DEPTH_TEST_IMAGE}")
        set_tests_properties(depth_runtime_regression PROPERTIES TIMEOUT 60 RESOURCE_LOCK npu LABELS hardware)
    endif()
endif()
