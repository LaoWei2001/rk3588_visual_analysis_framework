option(BUILD_FRAME_SNAPSHOT_TESTS "Build frame evidence and buffer ownership regression tests" OFF)
if(BUILD_FRAME_SNAPSHOT_TESTS)
    enable_testing()
    add_executable(event_retention_tests tests/event/test_event_retention.cpp ${cjson_src})
    target_include_directories(event_retention_tests PRIVATE ${PROJECT_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/src)
    target_compile_options(event_retention_tests PRIVATE -UNDEBUG)
    add_test(NAME event_retention_regression COMMAND event_retention_tests)
    add_executable(frame_snapshot_tests
        tests/event/test_frame_snapshot.cpp
        src/runtime/channel_snapshot.cpp
        src/pipeline/frame_transform.cpp
        src/pipeline/image_convert.cpp
        src/display/display.cpp
        src/display/text_overlay.cpp
    )
    target_include_directories(frame_snapshot_tests PRIVATE ${custom_inc} ${api_inc})
    target_compile_options(frame_snapshot_tests PRIVATE -UNDEBUG -ffunction-sections -fdata-sections)
    # 仅链接公共绘制函数，未使用的 GUI/实时播放入口不进入独立测试。
    set_target_properties(frame_snapshot_tests PROPERTIES LINK_FLAGS "-Wl,--gc-sections")
    target_link_libraries(frame_snapshot_tests PRIVATE ${OpenCV_LIBS} ${GSTOPT_LIBS} rga Threads::Threads)
    add_test(NAME frame_snapshot_regression COMMAND frame_snapshot_tests)
endif()

option(BUILD_DATASET_TESTS "Build dataset rule and remote collection regression tests" OFF)
if(BUILD_DATASET_TESTS)
    enable_testing()
    add_executable(dataset_rules_tests
        tests/dataset/test_rules.cpp
        src/dataset/rules.cpp
        ${cjson_src}
    )
    target_include_directories(dataset_rules_tests PRIVATE ${custom_inc} ${api_inc})
    target_compile_options(dataset_rules_tests PRIVATE -UNDEBUG)
    target_link_libraries(dataset_rules_tests PRIVATE ${OpenCV_LIBS} Threads::Threads)
    add_test(NAME dataset_rules_regression COMMAND dataset_rules_tests)
    add_executable(remote_dataset_tests tests/dataset/test_remote_dataset.cpp
        src/control/remote_dataset.cpp
        src/dataset/rules.cpp ${cjson_src})
    target_include_directories(remote_dataset_tests PRIVATE ${custom_inc} ${api_inc})
    target_compile_options(remote_dataset_tests PRIVATE -UNDEBUG)
    target_link_libraries(remote_dataset_tests PRIVATE ${OpenCV_LIBS} Threads::Threads)
    add_test(NAME remote_dataset_regression COMMAND remote_dataset_tests)
    set_tests_properties(remote_dataset_regression PROPERTIES TIMEOUT 20)
endif()

option(BUILD_CRANE_SAFETY_TESTS "Build crane horn GPIO timing regression tests" OFF)
if(BUILD_CRANE_SAFETY_TESTS)
    enable_testing()
    add_executable(crane_safety_tests
        tests/crane/test_safety_controller.cpp
        ${VISION_PROJECTS_ROOT}/global_modules/global_crane_safety_controller/safety_controller.cpp
    )
    target_include_directories(crane_safety_tests PRIVATE "${PROJECT_SOURCE_DIR}/include"
        ${VISION_PROJECTS_ROOT}/global_modules/global_crane_safety_controller)
    target_compile_options(crane_safety_tests PRIVATE -UNDEBUG)
    add_test(NAME crane_safety_regression COMMAND crane_safety_tests)
endif()

option(BUILD_CRANE_HOOK_TESTS "Build hook clipping, calibration and alarm regression tests" OFF)
if(BUILD_CRANE_HOOK_TESTS)
    enable_testing()
    add_executable(crane_hook_tests
        tests/crane/test_hook_geometry.cpp
        ${VISION_PROJECTS_ROOT}/modules/logic_crane_hook/hook_geometry.cpp
        ${VISION_PROJECTS_ROOT}/modules/logic_crane_hook/hook_guard.cpp
    )
    target_include_directories(crane_hook_tests PRIVATE ${custom_inc}
        ${VISION_PROJECTS_ROOT}/modules/logic_crane_hook)
    target_link_libraries(crane_hook_tests PRIVATE ${OpenCV_LIBS})
    target_compile_options(crane_hook_tests PRIVATE -UNDEBUG)
    add_test(NAME crane_hook_regression COMMAND crane_hook_tests)
endif()

option(BUILD_HELMET_TESTS "Build helmet evidence and alarm mode regression tests" OFF)
if(BUILD_HELMET_TESTS)
    enable_testing()
    add_executable(helmet_tests
        tests/helmet/test_helmet_guard.cpp
        ${VISION_PROJECTS_ROOT}/modules/logic_crane_helmet/helmet_guard.cpp
    )
    target_include_directories(helmet_tests PRIVATE ${custom_inc} ${api_inc}
        ${VISION_PROJECTS_ROOT}/modules/logic_crane_helmet)
    target_compile_options(helmet_tests PRIVATE -UNDEBUG)
    target_link_libraries(helmet_tests PRIVATE ${OpenCV_LIBS})
    add_test(NAME helmet_regression COMMAND helmet_tests)
endif()

option(BUILD_PPE_TESTS "Build PPE guard part-state and evidence window regression tests" OFF)
if(BUILD_PPE_TESTS)
    enable_testing()
    add_executable(ppe_tests
        tests/ppe/test_ppe_guard.cpp
        ${VISION_PROJECTS_ROOT}/modules/logic_ppe_guard/ppe_guard.cpp
    )
    target_include_directories(ppe_tests PRIVATE ${custom_inc} ${api_inc}
        ${VISION_PROJECTS_ROOT}/modules/logic_ppe_guard)
    target_compile_options(ppe_tests PRIVATE -UNDEBUG)
    target_link_libraries(ppe_tests PRIVATE ${OpenCV_LIBS})
    add_test(NAME ppe_regression COMMAND ppe_tests)
endif()
