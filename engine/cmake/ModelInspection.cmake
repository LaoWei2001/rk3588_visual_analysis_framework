# Metadata checks can be linked without image processing or video dependencies.
add_library(vision_model_inspection STATIC
    src/vision/models/model_probe.cpp
    src/vision/models/model_compatibility.cpp)
add_library(Vision::ModelInspection ALIAS vision_model_inspection)
target_compile_features(vision_model_inspection PUBLIC cxx_std_14)
target_include_directories(vision_model_inspection PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(vision_model_inspection PRIVATE rknnrt_vendor Threads::Threads)

option(BUILD_MODEL_INSPECTION_TESTS "Build SDK model metadata and decoder contract tests" OFF)
if(BUILD_MODEL_INSPECTION_TESTS)
    enable_testing()
    add_executable(model_inspection_contract_tests tests/inference/test_model_inspection.cpp)
    target_link_libraries(model_inspection_contract_tests PRIVATE Vision::ModelInspection)
    target_compile_options(model_inspection_contract_tests PRIVATE -UNDEBUG)
    add_test(NAME model_inspection_contracts COMMAND model_inspection_contract_tests)
    add_executable(model_probe_lifecycle_tests tests/inference/test_model_probe.cpp)
    target_include_directories(model_probe_lifecycle_tests PRIVATE ${RKNN_API_INCLUDE_DIR})
    target_link_libraries(model_probe_lifecycle_tests PRIVATE Vision::ModelInspection Threads::Threads)
    target_compile_options(model_probe_lifecycle_tests PRIVATE -UNDEBUG)
    add_test(NAME model_probe_lifecycle COMMAND model_probe_lifecycle_tests)
    set_tests_properties(model_inspection_contracts model_probe_lifecycle PROPERTIES TIMEOUT 15)
endif()
