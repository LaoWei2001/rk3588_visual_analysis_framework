#!/bin/bash

rm build -rf
mkdir build
cd build
cmake ..
make
cd ..
cp build/yolov11_track_demo yolov11_track_demo_release
