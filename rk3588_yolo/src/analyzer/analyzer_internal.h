/**
 * @file analyzer_internal.h
 * @brief analyzer 模块内部共享接口
 *
 * 仅供 analyzer/ 目录下各 .cpp 文件相互引用，不对外暴露。
 * 外部模块请使用 analyzer.h。
 *
 * 各文件职责速查:
 *   channel_pipeline.cpp  — 跟踪器 + invoke_channel_logic +
 * process_channel_results display_pipeline.cpp  — display_worker_thread
 *   frame_inlet.cpp       — videoOutHandle + FPS 节流 + RGA 转换
 *   result_dispatch.cpp   — dispatch_worker_thread
 *   analyzer.cpp          — 共享状态定义 + init/deinit + main 查询接口
 */
#pragma once

#include "../config/config.h"
#include "../core/app_ctrl.h"
#include "algoProcess.h"
#include <chrono>
#include <cstdint>
#include <cstdlib> /* malloc / free */
#include <opencv2/opencv.hpp>
#include <pthread.h>
#include <vector>

/*======================== 时间辅助 (各文件直接使用, 无链接冲突)
 * ========================*/

static inline uint64_t steady_now_ms(void)
{
    auto now = std::chrono::steady_clock::now();
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count());
}

static inline uint64_t steady_now_us(void)
{
    auto now = std::chrono::steady_clock::now();
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count());
}

/*========================
 * 三槽显示帧池（DispFramePool）========================*/

/**
 * @brief Triple-buffer 帧池：将帧像素拷贝从 mutex 保护区移到锁外。
 *
 * 问题背景
 * --------
 * 原来 DispTask 用 std::vector<uint8_t> data 存储完整帧像素，
 * videoOutHandle 的 data.assign() 在持 DispQueue::mtx 期间执行一次
 * 3 MB+ 的 memcpy，让显示线程被迫等待整段拷贝完成才能取帧。
 * 4 路 1080p NV12 @ 25fps ≈ 300 MB/s 的无效拷贝全在锁内发生。
 *
 * 解决方案（三槽轮转）
 * --------------------
 * 三个槽各司其职，角色在 back/mid/front 之间循环轮转：
 *
 *   slots[back_idx]   — 生产者当前写入槽（producer 独占，无需持锁访问）
 *   slots[mid_idx]    — 最新就绪帧（handoff 槽，publish/swap 时持
 * DispQueue::mtx） slots[front_idx]  — 消费者当前读取槽（consumer
 * 独占，无需持锁访问）
 *
 * 不变量：back_idx、mid_idx、front_idx 三者始终两两不等。
 *
 * 生产者工作流（frame_inlet.cpp）
 * --------------------------------
 *   1. back_buf(size)          — 取写入指针，容量不足时自动扩容，**无需持锁**
 *   2. memcpy(dst, imgData, n) — 3 MB 拷贝全程**无锁**
 *   3. 持 DispQueue::mtx
 *      → publish()             — back↔mid 整数交换（≈20 ns）
 *      → 释放锁，signal cv
 *
 * 消费者工作流（display_pipeline.cpp）
 * --------------------------------------
 *   1. 持 DispQueue::mtx
 *      → swap_front_if_dirty() — mid↔front 整数交换（≈10 ns）
 *      → 释放锁
 *   2. front_buf()             — **无锁**读取，生产者永不碰 front 槽
 *
 * 正确性
 * ------
 *   - publish()、swap_front_if_dirty() 均在同一把 DispQueue::mtx 下执行，
 *     mid 槽的所有权转移是原子的。
 *   - 生产者只写 slots[back_idx]，消费者只读 slots[front_idx]，
 *     back_idx ≠ front_idx 保证二者永不冲突，锁外访问安全。
 *   - back_buf() 读 back_idx：此值仅在持锁的 publish() 里改变。
 *     由于 videoOutHandle 是同一通道的单一生产者，publish() 返回后
 *     下一次 back_buf() 才被调用，两者严格顺序，无竞争。
 */
struct DispFramePool
{
    /** 每槽初始容量：NV12 1080p 典型值（horStride × verStride × 3/2）。
     *  实际分辨率更大时 back_buf() 会自动一次性扩容。*/
    static constexpr size_t INIT_BYTES = static_cast<size_t>(1920) * 1088 * 3 / 2;

    struct Slot
    {
        uint8_t *data = nullptr;
        size_t capacity = 0;
    };

    Slot slots[3];
    int back_idx = 0;  /* 生产者写入槽 */
    int mid_idx = 1;   /* 最新就绪槽   */
    int front_idx = 2; /* 消费者读取槽 */
    bool mid_dirty = false;

    /** 预分配三槽内存，由 analyzer_init 调用一次。*/
    void init()
    {
        for (auto &s : slots)
        {
            s.data = static_cast<uint8_t *>(malloc(INIT_BYTES));
            s.capacity = s.data ? INIT_BYTES : 0;
        }
    }

    /** 释放三槽内存，由 analyzer_destroy_display_queues 调用。*/
    void deinit()
    {
        for (auto &s : slots)
        {
            free(s.data);
            s.data = nullptr;
            s.capacity = 0;
        }
    }

    /**
     * @brief 返回生产者写入指针（back 槽）。
     *
     * 若当前槽容量不足则 realloc（仅在分辨率变化时触发，极少发生）。
     * 失败返回 nullptr，调用者应跳过本帧的显示入队。
     * 调用者无需持任何锁（producer 独占 back 槽）。
     */
    uint8_t *back_buf(size_t needed)
    {
        Slot &s = slots[back_idx];
        if (needed > s.capacity)
        {
            free(s.data);
            s.data = static_cast<uint8_t *>(malloc(needed));
            s.capacity = s.data ? needed : 0;
        }
        return s.data;
    }

    /**
     * @brief 生产者写完后调用：back↔mid 整数交换，标记新帧就绪。
     *
     * 调用者**必须**持 DispQueue::mtx。
     */
    void publish()
    {
        const int tmp = back_idx;
        back_idx = mid_idx;
        mid_idx = tmp;
        mid_dirty = true;
    }

    /**
     * @brief 消费者取帧前调用：若有新帧则 mid↔front 整数交换。
     *
     * 调用者**必须**持 DispQueue::mtx。
     * @return true 表示 front 已更新为最新帧，false 表示无新帧（front 不变）。
     */
    bool swap_front_if_dirty()
    {
        if (!mid_dirty)
            return false;
        const int tmp = mid_idx;
        mid_idx = front_idx;
        front_idx = tmp;
        mid_dirty = false;
        return true;
    }

    /**
     * @brief 返回消费者读取指针（front 槽）。
     *
     * 消费者独占 front 槽，调用者无需持锁。
     */
    const uint8_t *front_buf() const
    {
        return slots[front_idx].data;
    }
};

/*======================== 显示任务与队列 (frame_inlet + display_pipeline 共用)
 * ========================*/

/**
 * @brief 显示任务（仅元数据）。
 *
 * 帧像素数据由 DispQueue::pool（DispFramePool）管理，
 * 不在此结构体中持有，以避免在锁内拷贝数 MB 的帧数据。
 */
struct DispTask
{
    int chnId = -1;
    int srcFmt = 0;
    int srcWidth = 0;
    int srcHeight = 0;
    int srcHStride = 0;
    int srcVStride = 0;
};

/**
 * 单槽设计（不是队列）：
 * 新帧到达时若上帧未被取走，直接覆盖——始终保证屏幕显示最新解码帧。
 * 这是有意取舍（显示流畅性优先于帧不丢失），不是 bug。
 *
 * 帧像素数据由 pool 管理；持 mtx 的临界区只需完成整数级槽交换（≈20 ns），
 * 3 MB 的 memcpy 已移至锁外由生产者提前完成。
 */
struct DispQueue
{
    pthread_mutex_t mtx;
    pthread_cond_t cv;
    int has_task = 0;
    DispTask task;      /* 元数据：6 个整数 */
    DispFramePool pool; /* 帧像素：三槽预分配缓冲 */
};

/* 定义在 analyzer.cpp，frame_inlet 和 display_pipeline 共同访问 */
extern DispQueue g_disp_queues[MAX_CHANNEL_NUM];

/*======================== 分发线程运行标志 (定义在 analyzer.cpp)
 * ========================*/

extern volatile int g_dispatch_running;

/*======================== 通道结果处理串行锁 (定义在 analyzer.cpp)
 * ========================*/
/*
 * 防止同一通道的 process_channel_results 被两条路径并发调用：
 *   - 非推理通道：由 videoOutHandle (appsink 回调线程) 直接调用
 *   - 推理通道  ：由 dispatch_worker_thread 在 NPU 完成后调用
 * 两条路径对同一通道不应并发，此锁保证串行。
 */
extern pthread_mutex_t g_process_mtx[MAX_CHANNEL_NUM];

/*======================== 帧入口统计重置 (实现在 frame_inlet.cpp)
 * ========================*/

/**
 * @brief 重置指定通道的 FPS 节流计时器。
 *
 * 断流/重连时调用：使下一帧重新按 phase-offset 计算初始 due 时刻，
 * 避免旧计时器导致重连后长时间不送推理或立刻爆发。
 */
void feed_stats_reset(int chnId);

/*======================== 通道结果处理 (实现在 channel_pipeline.cpp)
 * ========================*/

std::vector<AlgoResult> process_channel_results(int chnId, const ChannelRawFrame &raw_frame,
                                                std::vector<AlgoResult> *new_results = nullptr,
                                                cv::Mat *infer_frame = nullptr, int64_t result_frame_id = 0);

/*======================== 跟踪器生命周期 (实现在 channel_pipeline.cpp)
 * ========================*/

void trackers_init(void);
void trackers_deinit(void);
