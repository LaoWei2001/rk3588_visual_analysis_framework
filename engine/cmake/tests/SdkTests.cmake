option(BUILD_SDK_TESTS "Validate public SDK without engine include paths" OFF)
if(BUILD_SDK_TESTS)
    enable_testing()
    set(sdk_test_args
        --compiler "${CMAKE_CXX_COMPILER}"
        --include "${PROJECT_SOURCE_DIR}/include"
        --projects "${VISION_PROJECTS_ROOT}"
    )
    foreach(directory IN LISTS OpenCV_INCLUDE_DIRS)
        list(APPEND sdk_test_args --opencv "${directory}")
    endforeach()
    add_test(NAME sdk_public_headers_regression
        COMMAND "${PYTHON3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/sdk/test_public_headers.py" ${sdk_test_args})
    set_tests_properties(sdk_public_headers_regression PROPERTIES TIMEOUT 180)

    add_library(sdk_target_query_consumer OBJECT tests/sdk/test_target_query.cpp)
    target_link_libraries(sdk_target_query_consumer PRIVATE Vision::SDK)
    target_compile_options(sdk_target_query_consumer PRIVATE -UNDEBUG)
    add_executable(sdk_target_query_tests
        $<TARGET_OBJECTS:sdk_target_query_consumer>
        src/logic/core/channel_logic.cpp
    )
    target_include_directories(sdk_target_query_tests PRIVATE ${custom_inc} ${api_inc})
    target_compile_options(sdk_target_query_tests PRIVATE -ffunction-sections -fdata-sections)
    set_target_properties(sdk_target_query_tests PROPERTIES LINK_FLAGS "-Wl,--gc-sections")
    target_link_libraries(sdk_target_query_tests PRIVATE ${OpenCV_LIBS} Threads::Threads)
    add_test(NAME sdk_target_query_regression COMMAND sdk_target_query_tests)
    set_tests_properties(sdk_target_query_regression PROPERTIES TIMEOUT 15)

    # The example is optional in externally supplied business workspaces.
    set(sdk_example "${VISION_PROJECTS_ROOT}/modules/logic_roi_dwell_demo/logic.cpp")
    if(EXISTS "${sdk_example}")
        add_library(sdk_example_consumer OBJECT "${sdk_example}")
        target_link_libraries(sdk_example_consumer PRIVATE Vision::SDK)
        add_executable(sdk_business_tests
            tests/sdk/test_business_workflow.cpp
            $<TARGET_OBJECTS:sdk_example_consumer>
            src/logic/core/channel_logic.cpp
            src/logic/core/logic_parameters.cpp
            src/display/display.cpp
            src/display/text_overlay.cpp
            "${logic_catalog_cpp}"
            ${cjson_src}
        )
        target_include_directories(sdk_business_tests PRIVATE ${custom_inc} ${api_inc})
        target_compile_options(sdk_business_tests PRIVATE -UNDEBUG -ffunction-sections -fdata-sections)
        set_target_properties(sdk_business_tests PROPERTIES LINK_FLAGS "-Wl,--gc-sections")
        target_link_libraries(sdk_business_tests PRIVATE ${OpenCV_LIBS} ${GTK3_LIBRARIES} Threads::Threads)
        file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/sdk-acceptance")
        add_test(NAME sdk_business_workflow_regression
            COMMAND sdk_business_tests "${CMAKE_CURRENT_BINARY_DIR}/sdk-acceptance")
        set_tests_properties(sdk_business_workflow_regression PROPERTIES TIMEOUT 30)
    endif()
endif()
