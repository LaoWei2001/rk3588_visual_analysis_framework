/**
 * @file pause_ctrl.cpp
 * @brief 全局暂停控制器实现
 *
 * 实现机制:
 *   使用 std::mutex + std::condition_variable 实现工作线程的精确阻塞/唤醒。
 *   g_paused 为 atomic<bool>，toggle() / wait_if_paused() 对其的读写均在锁内，
 *   保证 cond_var 不会丢失通知。
 *
 *   g_force_resume 为退出标志，signal_handler 或主线程调用 resume_all() 后置 true，
 *   所有阻塞中的 wait_if_paused() 立即返回，程序可以安全退出。
 */
#include "pause_ctrl.h"
#include <mutex>
#include <condition_variable>
#include <cstdio>

namespace pause_ctrl {

/*======================== 公开原子标志 ========================*/
std::atomic<bool> g_enabled{false};

/*======================== 内部状态 ========================*/
static std::mutex              g_mtx;
static std::condition_variable g_cv;
static std::atomic<bool>       g_paused{false};
static std::atomic<bool>       g_force_resume{false};

/*======================== 接口实现 ========================*/

void init(bool enabled)
{
    g_enabled.store(enabled, std::memory_order_seq_cst);
    g_paused.store(false, std::memory_order_seq_cst);
    g_force_resume.store(false, std::memory_order_seq_cst);
}

void toggle()
{
    if (!g_enabled.load(std::memory_order_relaxed))
        return;

    bool was_paused;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        was_paused = g_paused.load(std::memory_order_relaxed);
        g_paused.store(!was_paused, std::memory_order_relaxed);
    }
    /* 若从暂停切换到运行，需要唤醒所有阻塞中的工作线程 */
    if (was_paused)
    {
        g_cv.notify_all();
        printf("[Pause] RESUMED\n");
    }
    else
    {
        printf("[Pause] PAUSED  (press SPACE to resume)\n");
    }
}

void wait_if_paused()
{
    /* 快速路径: 功能未启用，或当前不在暂停状态 — 直接返回，零开销 */
    if (!g_enabled.load(std::memory_order_relaxed))
        return;
    if (!g_paused.load(std::memory_order_relaxed))
        return;
    if (g_force_resume.load(std::memory_order_relaxed))
        return;

    /* 进入阻塞等待 */
    std::unique_lock<std::mutex> lk(g_mtx);
    g_cv.wait(lk, [] {
        return !g_paused.load(std::memory_order_relaxed) ||
               g_force_resume.load(std::memory_order_relaxed);
    });
}

void resume_all()
{
    /* 程序退出时调用：先置强制恢复标志，再唤醒所有等待线程 */
    g_force_resume.store(true, std::memory_order_seq_cst);
    g_paused.store(false, std::memory_order_seq_cst);
    g_cv.notify_all();
}

bool is_paused()
{
    return g_enabled.load(std::memory_order_relaxed) &&
           g_paused.load(std::memory_order_relaxed);
}

} // namespace pause_ctrl
