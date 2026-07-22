#!/bin/bash
# ============================================================
# npu_freq.sh — RK3588 定频工具 (实验用: 测频率头寸能否抬高 NPU 天花板)
#
# 用法 (需 root):
#   sudo ./npu_freq.sh status    # 看当前 governor / 频率 / 温度 (基线)
#   sudo ./npu_freq.sh lock      # 锁 CPU/NPU/DDR 到最高频(userspace), 自动备份原 governor
#   sudo ./npu_freq.sh restore   # 恢复 lock 前的 governor
#
# 典型实验流程:
#   sudo ./npu_freq.sh status                       # 记基线频率+温度
#   sudo ./npu_freq.sh lock
#   sudo ./npu_bench ../assets/yolov8n.rknn 9 15    # 重测天花板, 和定频前的 196 比
#   sudo ./npu_freq.sh status                       # 确认频率确实上去了、温度多少
#   sudo ./npu_freq.sh restore                      # 实验完恢复
#
# ⚠ 锁最高频会抬高稳态功耗与发热。15s 基准安全; 但长时间生产部署前务必确认散热——
#   你日志里"15路跑~20h后帧数抖动、重启无效、关程序晾凉才恢复"是典型热积累(heat-soak)。
#   若要长期定频, 用下面的 NPU_FREQ/DMC_FREQ 环境变量锁到"可持续"档位而非绝对峰值, 例如:
#       NPU_FREQ=900000000 DMC_FREQ=1560000000 sudo ./npu_freq.sh lock
# ============================================================

SAVE=/tmp/npu_freq_saved.txt

# 取空格分隔频率列表里的最大值
max_of() { tr ' ' '\n' < "$1" 2>/dev/null | grep -E '^[0-9]+$' | sort -n | tail -1; }

# 找 NPU / DDR 的 devfreq 节点 (跨板名字可能不同, 用通配)
NPU_DEV=$(ls -d /sys/class/devfreq/*npu* 2>/dev/null | head -1)
DMC_DEV=$(ls -d /sys/class/devfreq/dmc   2>/dev/null | head -1)

show_temps() {
    for z in /sys/class/thermal/thermal_zone*; do
        [ -r "$z/temp" ] || continue
        local ty t
        ty=$(cat "$z/type" 2>/dev/null)
        t=$(cat "$z/temp" 2>/dev/null)
        awk -v ty="$ty" -v t="$t" 'BEGIN{printf "  %-14s %.1f C\n", ty, t/1000}'
    done
}

status() {
    echo "===== CPU ====="
    for p in /sys/devices/system/cpu/cpufreq/policy*; do
        [ -d "$p" ] || continue
        printf "  %s  gov=%-12s cur=%s kHz  max_avail=%s kHz\n" \
            "$(basename "$p")" \
            "$(cat "$p/scaling_governor" 2>/dev/null)" \
            "$(cat "$p/scaling_cur_freq" 2>/dev/null)" \
            "$(max_of "$p/scaling_available_frequencies")"
    done
    echo "===== NPU ($NPU_DEV) ====="
    if [ -n "$NPU_DEV" ]; then
        printf "  gov=%-12s cur=%s Hz  max_avail=%s Hz\n" \
            "$(cat "$NPU_DEV/governor" 2>/dev/null)" \
            "$(cat "$NPU_DEV/cur_freq" 2>/dev/null)" \
            "$(max_of "$NPU_DEV/available_frequencies")"
    fi
    echo "===== DDR ($DMC_DEV) ====="
    if [ -n "$DMC_DEV" ]; then
        printf "  gov=%-12s cur=%s Hz  max_avail=%s Hz\n" \
            "$(cat "$DMC_DEV/governor" 2>/dev/null)" \
            "$(cat "$DMC_DEV/cur_freq" 2>/dev/null)" \
            "$(max_of "$DMC_DEV/available_frequencies")"
    fi
    echo "===== 温度 ====="
    show_temps
}

lock() {
    : > "$SAVE"
    echo ">>> 备份当前 governor 到 $SAVE 并锁定最高频..."

    # ---- CPU: 每个 policy 锁到 userspace + 最高可用频 ----
    for p in /sys/devices/system/cpu/cpufreq/policy*; do
        [ -d "$p" ] || continue
        local g f
        g=$(cat "$p/scaling_governor" 2>/dev/null)
        echo "cpu|$p|$g" >> "$SAVE"
        f=${CPU_FREQ:-$(max_of "$p/scaling_available_frequencies")}
        echo userspace > "$p/scaling_governor" 2>/dev/null
        [ -n "$f" ] && echo "$f" > "$p/scaling_setspeed" 2>/dev/null
        echo "  $(basename "$p") -> $f kHz"
    done

    # ---- NPU ----
    if [ -n "$NPU_DEV" ]; then
        local g f
        g=$(cat "$NPU_DEV/governor" 2>/dev/null)
        echo "npu|$NPU_DEV|$g" >> "$SAVE"
        f=${NPU_FREQ:-$(max_of "$NPU_DEV/available_frequencies")}
        echo userspace > "$NPU_DEV/governor" 2>/dev/null
        [ -n "$f" ] && echo "$f" > "$NPU_DEV/userspace/set_freq" 2>/dev/null
        echo "  NPU -> $f Hz"
    fi

    # ---- DDR ----
    if [ -n "$DMC_DEV" ]; then
        local g f
        g=$(cat "$DMC_DEV/governor" 2>/dev/null)
        echo "dmc|$DMC_DEV|$g" >> "$SAVE"
        f=${DMC_FREQ:-$(max_of "$DMC_DEV/available_frequencies")}
        echo userspace > "$DMC_DEV/governor" 2>/dev/null
        [ -n "$f" ] && echo "$f" > "$DMC_DEV/userspace/set_freq" 2>/dev/null
        echo "  DDR -> $f Hz"
    fi

    echo ">>> 定频后状态:"
    status
}

restore() {
    if [ ! -f "$SAVE" ]; then
        echo "未找到备份 $SAVE — 无法自动恢复。可手动把 governor 设回 (CPU: schedutil, NPU/DDR: dmc_ondemand/simple_ondemand)。"
        return 1
    fi
    echo ">>> 从 $SAVE 恢复 governor..."
    while IFS='|' read -r kind path gov; do
        [ -n "$path" ] && [ -n "$gov" ] || continue
        case "$kind" in
            cpu) echo "$gov" > "$path/scaling_governor" 2>/dev/null && echo "  $(basename "$path") -> $gov" ;;
            npu|dmc) echo "$gov" > "$path/governor" 2>/dev/null && echo "  $kind -> $gov" ;;
        esac
    done < "$SAVE"
    rm -f "$SAVE"
    echo ">>> 恢复后状态:"
    status
}

case "${1:-status}" in
    status)  status ;;
    lock)    lock ;;
    restore) restore ;;
    *) echo "用法: sudo $0 {status|lock|restore}"; exit 1 ;;
esac
