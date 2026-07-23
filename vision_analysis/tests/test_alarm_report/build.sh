#!/bin/bash
set -e
cd "$(dirname "$0")"
rm -rf build
mkdir build
cd build
cmake ..
cmake --build . -j"$(nproc)"
./alarm_report_unit_test
