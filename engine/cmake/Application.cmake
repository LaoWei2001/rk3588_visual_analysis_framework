# The shipping executable is a consumer of the same public runtime as a single cpp.
add_executable(vision_analysis
    src/application/main.cpp
    src/application/tools/image_inference_cli.cpp
    "${logic_catalog_cpp}"
)
if(TARGET vision_business)
    target_sources(vision_analysis PRIVATE $<TARGET_OBJECTS:vision_business>)
endif()
target_include_directories(vision_analysis PRIVATE ${custom_inc} ${api_inc})
target_link_libraries(vision_analysis PRIVATE Vision::Runtime)

add_custom_target(npu_dispatch_audit
    COMMAND "${PYTHON3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/inference/check_npu_dispatch.py")
add_dependencies(vision_analysis npu_dispatch_audit)
set_target_properties(vision_analysis PROPERTIES
    BUILD_WITH_INSTALL_RPATH TRUE
    INSTALL_RPATH "$ORIGIN/libs"
)
