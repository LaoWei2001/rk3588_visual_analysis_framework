#!/bin/sh
set -eu
out=${1:?Usage: run.sh OUTPUT_DIRECTORY}
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH= cd -- "$here/../../../../.." && pwd)
build="$root/build/engine"
mkdir -p "$out"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
c++ -std=c++17 -O2 -I"$root/engine/include" $(pkg-config --cflags opencv4) \
    -x c++ "$here/replay.cpp.in" "$here/../../logic.cpp" "$here/../../ppe_guard.cpp" "$build/generated/logic_catalog_embedded.cpp" -x none \
    -L"$build" -lvision_analysis_sdk -lvision_inference -lvision_model_inspection -lvision_json \
    $(pkg-config --libs opencv4) -lpthread -o "$tmp/replay"
"$tmp/replay" "$tmp"
ffmpeg -loglevel error -y -i "$tmp/replay.avi" -c:v libx264 -crf 21 -pix_fmt yuv420p -movflags +faststart "$out/demo.mp4"
cp "$tmp/entry.jpg" "$tmp/timeout.jpg" "$out/"
