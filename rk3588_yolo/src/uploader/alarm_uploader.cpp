/**
 * @file alarm_uploader.cpp
 * @brief 报警上传模块 — C/pthread 风格
 *
 * 架构: 生产者-消费者模式
 * - alarm_uploader_enqueue() / dify_uploader_enqueue() 是生产者, 非阻塞
 * - upload_worker_thread (pthread) 是消费者, 负责 JPEG 编码 + Redis 写入
 */

#include "alarm_uploader.h"
#include "../core/base64_util.h"
#include <hiredis/hiredis.h>
#include <opencv2/imgcodecs.hpp>
#include <ctime>
#include <cstdio>
#include <cstring>
#include <pthread.h>
#include <queue>
#include <vector>
#include "../third_party/json/cJSON.h"

/*======================== 任务结构 ========================*/
struct AlarmTask
{
    cv::Mat img_draw;
    cv::Mat img_raw;
    int camera_id;
    std::string alarm_type;
    std::string snap_time;
    std::string server_url; /* 方案2: 该通道上报地址, 空=用默认 */
};

struct DifyTask
{
    cv::Mat img;
    std::string prompt;
    std::string event_id;
    std::string dify_api_url; /* 方案2: 该通道 Dify 地址, 空=用默认 */
    std::string dify_api_key; /* 方案2: 该通道 Dify 密钥, 空=用默认 */
};

/*======================== 内部状态 ========================*/
static redisContext *g_redis_ctx = nullptr;
static pthread_mutex_t g_redis_mtx = PTHREAD_MUTEX_INITIALIZER;

static std::queue<AlarmTask> g_alarm_queue;
static std::queue<DifyTask> g_dify_queue;
static pthread_mutex_t g_queue_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_queue_cv = PTHREAD_COND_INITIALIZER;

static volatile int g_uploader_running = 0;
static pthread_t g_upload_worker_tid;

/* 重连相关 */
static std::string g_redis_host;
static int g_redis_port = 6379;
static time_t g_last_reconnect_attempt = 0;

/*======================== 前向声明 ========================*/
static int redis_reconnect(void);

/*======================== 工具函数 ========================*/
static std::string mat_to_base64(const cv::Mat &img)
{
    if (img.empty())
        return "";
    std::vector<unsigned char> buff;
    cv::imencode(".jpg", img, buff, {cv::IMWRITE_JPEG_QUALITY, 90});
    return base64_encode(buff.data(), buff.size());
}

static std::string make_time_str(void)
{
    char buf[64];
    time_t now = time(NULL);
    struct tm t;
    localtime_r(&now, &t);
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &t);
    return std::string(buf);
}

static int redis_rpush(const char *queue_name, const char *json_payload, size_t len)
{
    pthread_mutex_lock(&g_redis_mtx);

    /* 上下文为空时尝试重连 */
    if (!g_redis_ctx)
    {
        if (!redis_reconnect())
        {
            pthread_mutex_unlock(&g_redis_mtx);
            return 0;
        }
    }

    redisReply *reply = (redisReply *)redisCommand(g_redis_ctx, "RPUSH %s %b",
                                                   queue_name, json_payload, len);
    if (!reply)
    {
        /* 命令失败 (连接断开等), 尝试重连后重试一次 */
        fprintf(stderr, "[alarm_uploader] RPUSH failed, attempting reconnect...\n");
        if (redis_reconnect())
        {
            reply = (redisReply *)redisCommand(g_redis_ctx, "RPUSH %s %b",
                                               queue_name, json_payload, len);
        }
    }

    if (!reply)
    {
        pthread_mutex_unlock(&g_redis_mtx);
        return 0;
    }
    freeReplyObject(reply);
    pthread_mutex_unlock(&g_redis_mtx);
    return 1;
}

static int redis_reconnect(void)
{
    /* 限流: 两次重连间隔至少 5 秒 */
    time_t now = time(NULL);
    if (now - g_last_reconnect_attempt < 5)
        return 0;
    g_last_reconnect_attempt = now;

    if (g_redis_ctx)
    {
        redisFree(g_redis_ctx);
        g_redis_ctx = nullptr;
    }

    g_redis_ctx = redisConnect(g_redis_host.c_str(), g_redis_port);
    if (!g_redis_ctx || g_redis_ctx->err)
    {
        fprintf(stderr, "[alarm_uploader] Redis reconnect failed: %s\n",
                g_redis_ctx ? (g_redis_ctx->errstr ? g_redis_ctx->errstr : "unknown") : "context is null");
        if (g_redis_ctx)
        {
            redisFree(g_redis_ctx);
            g_redis_ctx = nullptr;
        }
        return 0;
    }

    printf("[alarm_uploader] Redis reconnected successfully to %s:%d\n",
           g_redis_host.c_str(), g_redis_port);
    return 1;
}

static int build_and_push_server(const AlarmTask &task)
{
    cJSON *root = cJSON_CreateObject();
    if (!root)
        return 0;

    std::string b64_draw = mat_to_base64(task.img_draw);
    std::string b64_raw = mat_to_base64(task.img_raw);

    cJSON_AddNumberToObject(root, "timestamp", (double)time(NULL));
    cJSON_AddNumberToObject(root, "camera_id", task.camera_id);
    cJSON_AddStringToObject(root, "alarm_type", task.alarm_type.c_str());
    cJSON_AddStringToObject(root, "snapTime", task.snap_time.c_str());
    cJSON_AddStringToObject(root, "endTime", task.snap_time.c_str());
    cJSON_AddStringToObject(root, "base64Data", b64_draw.c_str());
    cJSON_AddStringToObject(root, "base64DataRaw", b64_raw.c_str());
    cJSON_AddStringToObject(root, "server_url", task.server_url.c_str()); /* 方案2: 地址随消息走 */

    char *json_str = cJSON_PrintUnformatted(root);
    int ok = 0;
    if (json_str)
    {
        ok = redis_rpush("server_queue", json_str, strlen(json_str));
        cJSON_free(json_str);
    }
    cJSON_Delete(root);
    return ok;
}

static int build_and_push_dify(const DifyTask &task)
{
    std::string b64 = mat_to_base64(task.img);

    cJSON *root = cJSON_CreateObject();
    if (!root)
        return 0;

    cJSON_AddStringToObject(root, "event_id", task.event_id.c_str());
    cJSON_AddStringToObject(root, "base64Data", b64.c_str());
    cJSON_AddStringToObject(root, "prompt", task.prompt.c_str());
    cJSON_AddStringToObject(root, "dify_api_url", task.dify_api_url.c_str()); /* 方案2: 地址随消息走 */
    cJSON_AddStringToObject(root, "dify_api_key", task.dify_api_key.c_str()); /* 方案2: 密钥随消息走 */

    char *json_str = cJSON_PrintUnformatted(root);
    int ok = 0;
    if (json_str)
    {
        ok = redis_rpush("dify_queue", json_str, strlen(json_str));
        cJSON_free(json_str);
    }
    cJSON_Delete(root);
    return ok;
}

/*======================== 上传工作线程 (pthread) ========================*/
static void *upload_worker_thread(void *arg)
{
    (void)arg;
    printf("[alarm_uploader] upload worker thread started\n");

    while (1)
    {
        AlarmTask alarm_task;
        DifyTask dify_task;
        int has_alarm = 0, has_dify = 0;

        pthread_mutex_lock(&g_queue_mtx);
        while (g_alarm_queue.empty() && g_dify_queue.empty() && g_uploader_running)
            pthread_cond_wait(&g_queue_cv, &g_queue_mtx);

        if (!g_alarm_queue.empty())
        {
            alarm_task = std::move(g_alarm_queue.front());
            g_alarm_queue.pop();
            has_alarm = 1;
        }
        if (!g_dify_queue.empty())
        {
            dify_task = std::move(g_dify_queue.front());
            g_dify_queue.pop();
            has_dify = 1;
        }
        pthread_mutex_unlock(&g_queue_mtx);

        if (!g_uploader_running && !has_alarm && !has_dify)
            break;

        if (has_alarm && !alarm_task.img_draw.empty())
        {
            if (!build_and_push_server(alarm_task))
                fprintf(stderr, "[alarm_uploader] WARNING: failed to push alarm for camera %d\n",
                        alarm_task.camera_id);
        }

        if (has_dify && !dify_task.img.empty())
        {
            if (!build_and_push_dify(dify_task))
                fprintf(stderr, "[alarm_uploader] WARNING: failed to push dify task %s\n",
                        dify_task.event_id.c_str());
        }
    }

    printf("[alarm_uploader] upload worker thread exiting\n");
    return nullptr;
}

/*======================== 公有接口实现 ========================*/
int alarm_uploader_init(const char *redis_host, int redis_port)
{
    /* 保存配置用于后续重连 */
    g_redis_host = redis_host ? redis_host : "127.0.0.1";
    g_redis_port = redis_port;

    pthread_mutex_lock(&g_redis_mtx);

    if (g_redis_ctx)
    {
        redisFree(g_redis_ctx);
        g_redis_ctx = nullptr;
    }

    int redis_ok = 1;
    g_redis_ctx = redisConnect(redis_host, redis_port);
    if (!g_redis_ctx || g_redis_ctx->err)
    {
        fprintf(stderr, "[alarm_uploader] Redis connection failed: %s\n",
                g_redis_ctx ? (g_redis_ctx->errstr ? g_redis_ctx->errstr : "unknown") : "context is null");
        if (g_redis_ctx)
        {
            redisFree(g_redis_ctx);
            g_redis_ctx = nullptr;
        }
        redis_ok = 0;
    }
    else
        printf("[alarm_uploader] Redis connected to %s:%d\n", redis_host, redis_port);
    pthread_mutex_unlock(&g_redis_mtx);

    /* 启动异步上传工作线程 (即使 Redis 暂时不可用也启动, worker 内会重连) */
    g_uploader_running = 1;
    int ret = pthread_create(&g_upload_worker_tid, nullptr, upload_worker_thread, nullptr);
    if (ret != 0)
    {
        fprintf(stderr, "[alarm_uploader] pthread_create failed: %s\n", strerror(ret));
        g_uploader_running = 0;
        return 0;
    }

    return redis_ok;
}

void alarm_uploader_deinit(void)
{
    g_uploader_running = 0;
    pthread_cond_signal(&g_queue_cv);
    pthread_join(g_upload_worker_tid, nullptr);

    /* 清空队列 */
    pthread_mutex_lock(&g_queue_mtx);
    while (!g_alarm_queue.empty())
        g_alarm_queue.pop();
    while (!g_dify_queue.empty())
        g_dify_queue.pop();
    pthread_mutex_unlock(&g_queue_mtx);

    /* 释放 Redis */
    pthread_mutex_lock(&g_redis_mtx);
    if (g_redis_ctx)
    {
        redisFree(g_redis_ctx);
        g_redis_ctx = nullptr;
    }
    pthread_mutex_unlock(&g_redis_mtx);
}

int alarm_uploader_enqueue(const cv::Mat &img_draw, const cv::Mat &img_raw,
                           int camera_id, const char *alarm_type, const char *server_url)
{
    static constexpr size_t MAX_QUEUE_SIZE = 32;

    AlarmTask task;
    task.img_draw = img_draw.clone();
    task.img_raw = img_raw.clone();
    task.camera_id = camera_id;
    task.alarm_type = alarm_type ? alarm_type : "";
    task.snap_time = make_time_str();
    task.server_url = server_url ? server_url : "";

    pthread_mutex_lock(&g_queue_mtx);
    if (g_alarm_queue.size() >= MAX_QUEUE_SIZE)
    {
        g_alarm_queue.pop();
        fprintf(stderr, "[alarm_uploader] WARNING: queue full, dropping oldest alarm\n");
    }
    g_alarm_queue.push(std::move(task));
    pthread_mutex_unlock(&g_queue_mtx);
    pthread_cond_signal(&g_queue_cv);
    return 1;
}

int dify_uploader_enqueue(const cv::Mat &img, const char *prompt, const char *event_id,
                          const char *dify_api_url, const char *dify_api_key)
{
    static constexpr size_t MAX_DIFY_QUEUE = 16;

    if (img.empty())
        return 0;

    DifyTask task;
    task.img = img.clone();
    task.prompt = prompt ? prompt : "";
    task.event_id = event_id ? event_id : "";
    task.dify_api_url = dify_api_url ? dify_api_url : "";
    task.dify_api_key = dify_api_key ? dify_api_key : "";

    pthread_mutex_lock(&g_queue_mtx);
    if (g_dify_queue.size() >= MAX_DIFY_QUEUE)
    {
        g_dify_queue.pop();
        fprintf(stderr, "[alarm_uploader] WARNING: dify queue full, dropping oldest\n");
    }
    g_dify_queue.push(std::move(task));
    pthread_mutex_unlock(&g_queue_mtx);
    pthread_cond_signal(&g_queue_cv);
    return 1;
}
