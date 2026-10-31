# 跟踪器回归测试不依赖 RKNN/GStreamer 运行时，默认不纳入产品构建。
option(BUILD_TRACKER_TESTS "Build standalone tracker regression tests" OFF)
if(BUILD_TRACKER_TESTS)
    enable_testing()
    add_executable(tracker_tests
        tests/tracking/test_tracker.cpp
        src/tracking/tracker.cpp
    )
    target_include_directories(tracker_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/include
        ${OpenCV_INCLUDE_DIRS}
        ${PROJECT_SOURCE_DIR}/src
    )
    target_link_libraries(tracker_tests PRIVATE ${OpenCV_LIBS})
    add_test(NAME tracker_regression COMMAND tracker_tests)

    add_executable(bytetrack_tests
        tests/tracking/test_bytetrack.cpp
        src/tracking/bytetrack.cpp
    )
    target_include_directories(bytetrack_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/include
        ${OpenCV_INCLUDE_DIRS}
        ${PROJECT_SOURCE_DIR}/src
    )
    target_link_libraries(bytetrack_tests PRIVATE ${OpenCV_LIBS})
    add_test(NAME bytetrack_regression COMMAND bytetrack_tests)
endif()

option(BUILD_INFERENCE_ROI_TESTS "Build standalone inference ROI filter tests" OFF)
option(BUILD_MODEL_METADATA_TESTS "Build CPU-only RKNN metadata and numeric validation tests" OFF)
if(BUILD_MODEL_METADATA_TESTS)
    enable_testing()
    add_executable(model_metadata_tests tests/inference/test_model_metadata.cpp)
    target_include_directories(model_metadata_tests PRIVATE src "${RKNN_API_INCLUDE_DIR}")
    target_compile_features(model_metadata_tests PRIVATE cxx_std_11)
    target_compile_options(model_metadata_tests PRIVATE -UNDEBUG)
    add_test(NAME model_metadata_regression COMMAND model_metadata_tests)
endif()
if(BUILD_INFERENCE_ROI_TESTS)
    enable_testing()
    add_executable(inference_roi_filter_tests
        tests/inference/test_inference_roi_filter.cpp
        src/inference/inference_roi_filter.cpp
        src/inference/inference_roi_geometry.cpp
    )
    target_include_directories(inference_roi_filter_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/include
        ${OpenCV_INCLUDE_DIRS}
        ${PROJECT_SOURCE_DIR}/src
    )
    target_compile_options(inference_roi_filter_tests PRIVATE -UNDEBUG)
    target_link_libraries(inference_roi_filter_tests PRIVATE ${OpenCV_LIBS})
    add_test(NAME inference_roi_filter_regression COMMAND inference_roi_filter_tests)

    add_executable(business_coordinates_tests
        tests/inference/test_business_coordinates.cpp
        src/inference/inference_result_mapping.cpp
        src/inference/inference_roi_geometry.cpp
        src/inference/inference_roi_filter.cpp
        src/pipeline/frame_transform.cpp
        src/pipeline/image_convert.cpp
        src/logic/core/channel_logic.cpp
        src/display/display.cpp
        src/display/text_overlay.cpp
    )
    target_include_directories(business_coordinates_tests PRIVATE ${custom_inc} ${api_inc} ${RKNN_API_INCLUDE_DIR})
    target_compile_options(business_coordinates_tests PRIVATE -UNDEBUG -ffunction-sections -fdata-sections)
    set_target_properties(business_coordinates_tests PROPERTIES LINK_FLAGS "-Wl,--gc-sections")
    target_link_libraries(business_coordinates_tests PRIVATE ${OpenCV_LIBS} ${GSTOPT_LIBS} rga Threads::Threads)
    add_test(NAME business_coordinates_regression COMMAND business_coordinates_tests)

    add_executable(model_status_tests
        tests/inference/test_model_status.cpp
        src/inference/model_status.cpp
        src/yolo/composite_model.cpp
        ${cjson_src}
    )
    target_include_directories(model_status_tests PRIVATE ${custom_inc})
    target_compile_options(model_status_tests PRIVATE -UNDEBUG)
    target_link_libraries(model_status_tests PRIVATE ${OpenCV_LIBS} Threads::Threads)
    add_test(NAME model_status_regression COMMAND model_status_tests)
endif()
