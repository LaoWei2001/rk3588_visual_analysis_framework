cmake_minimum_required(VERSION 3.10)
project(alarm_report_unit_test CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(OpenCV REQUIRED)
find_package(Threads REQUIRED)

add_executable(alarm_report_unit_test
    alarm_report_unit_test.cpp
    ../../src/alarm/alarm_report.cpp
    ../../src/third_party/json/cJSON.c
)

target_include_directories(alarm_report_unit_test PRIVATE
    ../../src
    ../../src/third_party/json
    ${OpenCV_INCLUDE_DIRS}
)

target_link_libraries(alarm_report_unit_test ${OpenCV_LIBS} Threads::Threads)
