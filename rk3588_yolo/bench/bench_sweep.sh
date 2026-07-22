#!/bin/bash
# 扫描 contexts 数量, 找到 NPU 吞吐的"拐点"(再加 context 也不涨 = 已撞墙).
# 用法: ./bench_sweep.sh <model.rknn> [seconds_each=10] [core_mode=0] [readback=0]
set -e

MODEL="${1:?用法: ./bench_sweep.sh <model.rknn> [seconds=10] [core_mode=0] [readback=0]}"
SECS="${2:-10}"
CORE_MODE="${3:-0}"
READBACK="${4:-0}"
BIN="$(dirname "$0")/npu_bench"

[ -x "$BIN" ] || { echo "未找到 $BIN, 先编译: g++ -O3 -std=c++14 npu_bench.cpp -o npu_bench -lrknnrt -lpthread"; exit 1; }

echo "模型: $MODEL | 每档 ${SECS}s | core_mode=$CORE_MODE | readback=$READBACK"
echo "------------------------------------------"
printf "%-10s %-12s %-12s\n" "contexts" "总FPS" "单核FPS"
echo "------------------------------------------"
for N in 1 2 3 6 9 12 15; do
    OUT=$("$BIN" "$MODEL" "$N" "$SECS" "$CORE_MODE" "$READBACK" 2>/dev/null)
    FPS=$(echo "$OUT"     | grep "总吞吐"   | grep -oE '[0-9]+\.[0-9]+' | head -1)
    PERCORE=$(echo "$OUT" | grep "单核均摊" | grep -oE '[0-9]+\.[0-9]+' | head -1)
    printf "%-10s %-12s %-12s\n" "$N" "${FPS:-?}" "${PERCORE:-?}"
done
echo "------------------------------------------"
echo "解读: 总FPS 不再随 contexts 上涨的那一档 = NPU 已满载, 就是你的天花板."
