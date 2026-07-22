/**
 * @file npu_bench.cpp
 * @brief 裸 NPU 吞吐基准 —— 标定本机 NPU 算力天花板
 *
 * 剥掉整条 CPU 流水线 (GStreamer 解码 / RGA 转换 / 后处理解码 / NMS / 调度 / 队列),
 * 只在多个 rknn_context 上死循环 rknn_run, 用来回答一个问题:
 *
 *     "我那 12 帧/路 × 15 路 ≈ 180 次/秒, 到底是撞了 NPU 算力墙,
 *      还是卡在 DDR / 后处理 / 调度?"
 *
 * 用法:
 *   ./npu_bench <model.rknn> [contexts=12] [seconds=15] [core_mode=0] [readback=0]
 *
 *   contexts   并发 NPU context 数 (= worker 线程数, 1:1). 关键扫描变量.
 *              你的真实部署 15 路 = 15 context 跑在 3 核, 想复刻就传 15.
 *              想找拐点就从 1,3,6,9,12,15 扫一遍 (见 bench_sweep.sh).
 *   seconds    测量窗口秒数.
 *   core_mode  0 = 单核轮询绑定 (ctx_idx % 3)  —— 复刻你 algoProcess.cpp 的绑核方式 [默认]
 *              1 = 所有 ctx 用 CORE_0_1_2     (driver 把每次推理自动摊到 3 核)
 *              2 = 所有 ctx 用 CORE_AUTO       (driver 自行调度)
 *   readback   0 = 纯 rknn_run, 输入+输出全零拷贝绑定 —— 纯 NPU 天花板 [默认]
 *              1 = rknn_run + rknn_outputs_get + release —— 复刻你 infer_zero_copy 的
 *                  每帧 NPU 侧真实开销 (含一次输出读回拷贝).
 *              (0 与 1 之差 = rknn_outputs_get 读回成本)
 *              (1 与你真实流水线 FPS 之差 = RGA + 后处理 + NMS + 调度 + 解码 的总开销)
 *
 * 输入绑定严格对齐你的 yolo.cpp::init_zero_copy_input():
 *   type = UINT8, fmt = NHWC, pass_through = 0 (driver 做 normalize).
 *   ——故意不抄参考仓库的 pass_through=1, 否则测出来的天花板对不上你的真实模型.
 *
 * 编译 (板端 aarch64, 最简):
 *   g++ -O3 -std=c++14 npu_bench.cpp -o npu_bench -lrknnrt -lpthread
 *
 * 运行 (建议先定频 + root, 这样能读到 NPU 占用):
 *   ./npu_bench assets/yolov8n.rknn 12 15
 */

#include <rknn_api.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

static constexpr int NUM_CORES = 3; // RK3588 物理 NPU 核数, 固定

/* 运行期全局开关 / 计数 */
static std::atomic<bool> g_stop{false};
static std::atomic<long> g_total{0};      // 仅供每秒实时监视, relaxed, 频率极低无争用
static bool              g_readback = false;

/* 每线程独占, 循环结束时写回, 无锁无争用 */
static std::vector<long>   g_counts;   // 每个 context 的推理次数
static std::vector<double> g_busy_ns;  // 每个 context 的 rknn_run 累计阻塞时间(ns)

/*======================== 单个 context 的资源 ========================*/
struct BenchCtx
{
    rknn_context              ctx        = 0;
    rknn_tensor_mem          *in_mem     = nullptr;
    std::vector<rknn_tensor_mem *> out_mems;   // 仅 ceiling 模式绑定
    int                       core       = -1; // 绑定的物理核(单核模式), -1 = driver 调度
    int                       n_out      = 0;
    bool                      is_quant   = false;
};

/*======================== 工具 ========================*/

static unsigned char *load_file(const char *path, int *out_size)
{
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return nullptr;
    int sz = static_cast<int>(f.tellg());
    f.seekg(0);
    auto *buf = static_cast<unsigned char *>(malloc(sz));
    if (!buf) return nullptr;
    f.read(reinterpret_cast<char *>(buf), sz);
    *out_size = sz;
    return buf;
}

/* 读 RK3588 NPU 实时占用 (需 root + debugfs). 读不到返回空串. */
static std::string read_npu_load()
{
    std::ifstream f("/sys/kernel/debug/rknpu/load");
    if (!f) return std::string();
    std::string line;
    std::getline(f, line);
    /* 去掉行内多余空白, 紧凑成一行 */
    std::string out;
    bool prev_space = false;
    for (char c : line)
    {
        if (c == '\n' || c == '\r') continue;
        bool sp = (c == ' ' || c == '\t');
        if (sp && prev_space) continue;
        out.push_back(sp ? ' ' : c);
        prev_space = sp;
    }
    return out;
}

/*======================== 单 context 初始化 ========================*/

static bool setup_ctx(BenchCtx &bc, unsigned char *model_data, int model_size,
                      int ctx_idx, int core_mode)
{
    int ret = rknn_init(&bc.ctx, model_data, model_size, 0, nullptr);
    if (ret < 0)
    {
        fprintf(stderr, "[ctx%d] rknn_init failed: %d\n", ctx_idx, ret);
        return false;
    }

    /* 绑核策略 —— 与 algoProcess.cpp 对齐 */
    rknn_core_mask single[NUM_CORES] = {RKNN_NPU_CORE_0, RKNN_NPU_CORE_1, RKNN_NPU_CORE_2};
    rknn_core_mask mask;
    if (core_mode == 1)      { mask = RKNN_NPU_CORE_0_1_2; bc.core = -1; }
    else if (core_mode == 2) { mask = RKNN_NPU_CORE_AUTO;  bc.core = -1; }
    else                     { bc.core = ctx_idx % NUM_CORES; mask = single[bc.core]; }
    if (rknn_set_core_mask(bc.ctx, mask) < 0)
        fprintf(stderr, "[ctx%d] warn: set_core_mask failed\n", ctx_idx);

    rknn_input_output_num io_num;
    if (rknn_query(bc.ctx, RKNN_QUERY_IN_OUT_NUM, &io_num, sizeof(io_num)) < 0) return false;
    bc.n_out = io_num.n_output;

    rknn_tensor_attr in_attr;
    memset(&in_attr, 0, sizeof(in_attr));
    in_attr.index = 0;
    if (rknn_query(bc.ctx, RKNN_QUERY_INPUT_ATTR, &in_attr, sizeof(in_attr)) < 0) return false;
    const uint32_t W = in_attr.dims[2], H = in_attr.dims[1], C = in_attr.dims[3];

    /* 量化判定 —— 与 yolo.cpp::query_model_info() 同逻辑 */
    rknn_tensor_attr out0;
    memset(&out0, 0, sizeof(out0));
    out0.index = 0;
    rknn_query(bc.ctx, RKNN_QUERY_OUTPUT_ATTR, &out0, sizeof(out0));
    bc.is_quant = (out0.qnt_type == RKNN_TENSOR_QNT_AFFINE_ASYMMETRIC &&
                   out0.type != RKNN_TENSOR_FLOAT16);

    /* 输入零拷贝内存 —— 与 init_zero_copy_input() 完全一致 */
    uint32_t plain = W * H * C;
    uint32_t alloc = plain > in_attr.size_with_stride ? plain : in_attr.size_with_stride;
    bc.in_mem = rknn_create_mem(bc.ctx, alloc);
    if (!bc.in_mem || !bc.in_mem->virt_addr)
    {
        fprintf(stderr, "[ctx%d] rknn_create_mem(input) failed\n", ctx_idx);
        return false;
    }
    memset(bc.in_mem->virt_addr, 114, alloc); // 填 letterbox 灰; NPU 计时与内容无关

    rknn_tensor_attr in_io = in_attr;
    in_io.index        = 0;
    in_io.type         = RKNN_TENSOR_UINT8;
    in_io.fmt          = RKNN_TENSOR_NHWC;
    in_io.pass_through = 0;   // ←★ 对齐你的真实设置 (driver 做 normalize), 不是参考仓库的 1
    in_io.h_stride     = 0;
    if (rknn_set_io_mem(bc.ctx, bc.in_mem, &in_io) < 0)
    {
        fprintf(stderr, "[ctx%d] rknn_set_io_mem(input) failed\n", ctx_idx);
        return false;
    }

    /* ceiling 模式: 输出也走零拷贝绑定, 循环里只 rknn_run, 不读回.
     * readback 模式: 不绑输出, 循环里用 rknn_outputs_get 读回(复刻真实流水线). */
    if (!g_readback)
    {
        for (int o = 0; o < bc.n_out; ++o)
        {
            rknn_tensor_attr oa;
            memset(&oa, 0, sizeof(oa));
            oa.index = o;
            rknn_query(bc.ctx, RKNN_QUERY_OUTPUT_ATTR, &oa, sizeof(oa));
            rknn_tensor_mem *m = rknn_create_mem(bc.ctx, oa.size_with_stride);
            if (!m) { fprintf(stderr, "[ctx%d] create_mem(out%d) failed\n", ctx_idx, o); return false; }
            if (rknn_set_io_mem(bc.ctx, m, &oa) < 0)
            { fprintf(stderr, "[ctx%d] set_io_mem(out%d) failed\n", ctx_idx, o); return false; }
            bc.out_mems.push_back(m);
        }
    }
    return true;
}

/*======================== worker ========================*/

static void worker_loop(int tid, BenchCtx *bc)
{
    long   local   = 0;
    double busy_ns = 0.0;

    std::vector<rknn_output> outs;
    if (g_readback) outs.resize(bc->n_out);

    while (!g_stop.load(std::memory_order_relaxed))
    {
        auto t0 = std::chrono::steady_clock::now();

        if (rknn_run(bc->ctx, nullptr) < 0)
        {
            fprintf(stderr, "[ctx%d] rknn_run failed, worker exit\n", tid);
            break;
        }

        if (g_readback)
        {
            for (int i = 0; i < bc->n_out; ++i)
            {
                memset(&outs[i], 0, sizeof(rknn_output));
                outs[i].want_float = bc->is_quant ? 0 : 1; // 对齐 want_float = !is_quant
            }
            if (rknn_outputs_get(bc->ctx, bc->n_out, outs.data(), nullptr) >= 0)
                rknn_outputs_release(bc->ctx, bc->n_out, outs.data());
        }

        auto t1 = std::chrono::steady_clock::now();
        busy_ns += std::chrono::duration<double, std::nano>(t1 - t0).count();
        ++local;
        g_total.fetch_add(1, std::memory_order_relaxed);
    }

    g_counts[tid]  = local;
    g_busy_ns[tid] = busy_ns;
}

/*======================== main ========================*/

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        printf("用法: %s <model.rknn> [contexts=12] [seconds=15] [core_mode=0] [readback=0]\n", argv[0]);
        printf("  core_mode: 0=单核轮询绑定(i%%3)  1=CORE_0_1_2  2=CORE_AUTO\n");
        printf("  readback : 0=纯rknn_run(天花板)  1=带outputs_get(真实每帧NPU侧开销)\n");
        return 1;
    }
    const char *model_path = argv[1];
    int contexts  = (argc > 2) ? atoi(argv[2]) : 12;
    int seconds   = (argc > 3) ? atoi(argv[3]) : 15;
    int core_mode = (argc > 4) ? atoi(argv[4]) : 0;
    g_readback    = (argc > 5) ? (atoi(argv[5]) != 0) : false;
    if (contexts < 1) contexts = 1;
    if (seconds  < 1) seconds  = 1;
    if (core_mode < 0 || core_mode > 2) core_mode = 0;

    int model_size = 0;
    unsigned char *model_data = load_file(model_path, &model_size);
    if (!model_data) { printf("模型加载失败: %s\n", model_path); return 1; }

    std::vector<BenchCtx> ctxs(contexts);
    for (int i = 0; i < contexts; ++i)
        if (!setup_ctx(ctxs[i], model_data, model_size, i, core_mode))
        { printf("ctx %d 初始化失败, 退出\n", i); return 1; }

    g_counts.assign(contexts, 0);
    g_busy_ns.assign(contexts, 0.0);

    const char *mode_name[] = {"单核轮询绑定(i%3)", "CORE_0_1_2", "CORE_AUTO"};
    printf("\n===== NPU 裸吞吐基准 =====\n");
    printf("模型      : %s (%d bytes)\n", model_path, model_size);
    printf("量化      : %s\n", ctxs[0].is_quant ? "int8 (is_quant=1)" : "float");
    printf("contexts  : %d (= 线程数)\n", contexts);
    printf("绑核模式  : %d = %s\n", core_mode, mode_name[core_mode < 3 ? core_mode : 0]);
    printf("测量模式  : %s\n", g_readback ? "readback (rknn_run + outputs_get)" : "ceiling (纯 rknn_run)");
    printf("窗口      : %d s\n", seconds);
    {
        std::string ld = read_npu_load();
        printf("NPU占用读取: %s\n", ld.empty() ? "不可用 (需 root + debugfs)" : "可用");
    }
    printf("==========================\n\n");

    /* 起线程 */
    std::vector<std::thread> ths;
    ths.reserve(contexts);
    auto t_start = std::chrono::steady_clock::now();
    for (int i = 0; i < contexts; ++i) ths.emplace_back(worker_loop, i, &ctxs[i]);

    /* 每秒监视: 瞬时 FPS + NPU 占用 */
    long   last_total = 0;
    auto   last_tp    = t_start;
    for (int s = 0; s < seconds; ++s)
    {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        auto now      = std::chrono::steady_clock::now();
        long cur      = g_total.load(std::memory_order_relaxed);
        double dt     = std::chrono::duration<double>(now - last_tp).count();
        double inst   = dt > 0 ? (cur - last_total) / dt : 0;
        std::string ld = read_npu_load();
        printf("[%2ds] 累计 %-7ld 瞬时 %6.1f FPS%s%s\n",
               s + 1, cur, inst,
               ld.empty() ? "" : "  | ", ld.c_str());
        fflush(stdout);
        last_total = cur;
        last_tp    = now;
    }

    g_stop.store(true, std::memory_order_relaxed);
    for (auto &t : ths) t.join();
    auto t_end = std::chrono::steady_clock::now();

    /* 汇总 */
    double elapsed = std::chrono::duration<double>(t_end - t_start).count();
    long   total   = 0;
    for (long c : g_counts) total += c;
    double fps = total / elapsed;

    printf("\n===== 结果 =====\n");
    printf("总推理次数: %ld\n", total);
    printf("实际耗时  : %.3f s\n", elapsed);
    printf("总吞吐    : \033[1;32m%.2f FPS\033[0m", fps);
    printf("%s\n", g_readback ? "   (真实每帧 NPU 侧, 含读回)" : "   ← 本机 NPU 天花板");
    printf("单核均摊  : %.2f FPS/core\n", fps / NUM_CORES);

    /* 每 context 平均 rknn_run 阻塞延迟(含同核排队) */
    double avg_lat_sum = 0; int avg_n = 0;
    for (int i = 0; i < contexts; ++i)
        if (g_counts[i] > 0) { avg_lat_sum += g_busy_ns[i] / g_counts[i] / 1e6; ++avg_n; }
    if (avg_n) printf("单次rknn_run: %.2f ms (单context阻塞延迟, 含同核排队)\n", avg_lat_sum / avg_n);

    if (core_mode == 0)
    {
        printf("--- 每核分布 ---\n");
        for (int c = 0; c < NUM_CORES; ++c)
        {
            long cc = 0;
            for (int i = 0; i < contexts; ++i) if (ctxs[i].core == c) cc += g_counts[i];
            printf("  Core%d: %-7ld (%.1f FPS)\n", c, cc, cc / elapsed);
        }
    }

    /* 清理 */
    for (auto &bc : ctxs)
    {
        for (auto *m : bc.out_mems) rknn_destroy_mem(bc.ctx, m);
        if (bc.in_mem) rknn_destroy_mem(bc.ctx, bc.in_mem);
        if (bc.ctx) rknn_destroy(bc.ctx);
    }
    free(model_data);
    return 0;
}
