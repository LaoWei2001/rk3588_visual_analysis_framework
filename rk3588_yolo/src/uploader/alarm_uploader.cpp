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
#include <string>
#include "../third_party/json/cJSON.h"

/* 本地发件箱落盘所需 (POSIX, 板端 Linux 原生可用) */
#include <cstdlib>     /* getenv, realpath */
#include <cerrno>
#include <climits>     /* PATH_MAX */
#include <sys/time.h>  /* gettimeofday */
#include <sys/stat.h>  /* mkdir, stat, S_ISREG */
#include <sys/types.h>
#include <dirent.h>    /* opendir/readdir */
#include <unistd.h>    /* unlink */
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

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

/*======================== 本地告警发件箱 (server 路径: 落盘, 不再走 Redis) ========================
 * 设计: 报警先无条件落地到本地"发件箱"目录(带框图 + 原图 + .json 元数据),
 *       由 Python 上报微服务扫描该目录补传到平台, 传成功即删除。
 *       断网 / 微服务没开时数据留在本地、网页可见; 通了就边传边删。
 * 上限: 本地占用超过 ALARM_STORE_CAP_BYTES 时, 从最老的记录开始删, 保证新报警记得下。
 * 注意: Dify 路径仍走 Redis(下方 build_and_push_dify 不变)。 */
static const long long ALARM_STORE_CAP_BYTES = 100LL * 1024 * 1024; /* 100 MB */

/* 递归创建目录 (mkdir -p)。成功返回 0。 */
static int mkdir_p(const std::string &dir)
{
    std::string cur;
    if (!dir.empty() && dir[0] == '/')
        cur = "/";
    size_t start = 0;
    while (start < dir.size())
    {
        size_t pos = dir.find('/', start);
        std::string seg = (pos == std::string::npos) ? dir.substr(start) : dir.substr(start, pos - start);
        if (!seg.empty())
        {
            if (cur.empty())     cur = seg;
            else if (cur == "/") cur += seg;
            else                 cur += "/" + seg;
            if (::mkdir(cur.c_str(), 0755) != 0 && errno != EEXIST)
                return -1;
        }
        if (pos == std::string::npos)
            break;
        start = pos + 1;
    }
    return 0;
}

/* 取发件箱目录(env ALARM_STORE_DIR, 默认 ./alarm_store), 首次调用创建并打印绝对路径。
 * 仅由唯一的上传线程调用, 静态初始化无需加锁。 */
static const std::string &get_store_dir(void)
{
    static std::string dir;
    static bool inited = false;
    if (!inited)
    {
        const char *env = getenv("ALARM_STORE_DIR");
        dir = (env && env[0]) ? env : "./alarm_store";
        if (mkdir_p(dir) != 0)
        {
            fprintf(stderr, "[alarm_recorder] 无法创建发件箱目录 '%s' (errno=%d), 回退到 ./alarm_store\n",
                    dir.c_str(), errno);
            dir = "./alarm_store";
            mkdir_p(dir);
        }
        char abs[PATH_MAX];
        if (realpath(dir.c_str(), abs))
            printf("[alarm_recorder] 本地告警发件箱: %s (上限 %lld MB)\n",
                   abs, ALARM_STORE_CAP_BYTES / (1024 * 1024));
        else
            printf("[alarm_recorder] 本地告警发件箱: %s\n", dir.c_str());
        inited = true;
    }
    return dir;
}

/* 删除一条记录的全部文件(json + 带框图 + 原图), 缺失忽略。 */
static void delete_record_files(const std::string &dir, const std::string &base)
{
    ::unlink((dir + "/" + base + ".json").c_str());
    ::unlink((dir + "/" + base + ".jpg").c_str());
    ::unlink((dir + "/" + base + "_raw.jpg").c_str());
}

/* 发件箱超过上限时, 反复删最老的记录(按 .json 的 mtime), 直到回到上限内。 */
static void enforce_store_cap(const std::string &dir)
{
    for (int guard = 0; guard < 1000000; ++guard)
    {
        long long total = 0;
        std::string oldest_base;
        time_t oldest_mtime = 0;
        bool found = false;

        DIR *d = opendir(dir.c_str());
        if (!d)
            return;
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL)
        {
            if (ent->d_name[0] == '.')
                continue;
            std::string name = ent->d_name;
            struct stat st;
            if (stat((dir + "/" + name).c_str(), &st) != 0 || !S_ISREG(st.st_mode))
                continue;
            total += (long long)st.st_size;
            if (name.size() > 5 && name.compare(name.size() - 5, 5, ".json") == 0)
            {
                if (!found || st.st_mtime < oldest_mtime)
                {
                    found = true;
                    oldest_mtime = st.st_mtime;
                    oldest_base = name.substr(0, name.size() - 5);
                }
            }
        }
        closedir(d);

        if (total <= ALARM_STORE_CAP_BYTES || !found)
            return;

        delete_record_files(dir, oldest_base);
        fprintf(stderr, "[alarm_recorder] 发件箱超过 %lld MB, 删除最老记录: %s\n",
                ALARM_STORE_CAP_BYTES / (1024 * 1024), oldest_base.c_str());
    }
}

/* 把一条 server 告警落地到本地发件箱: 写 带框图 + 原图 + .json 元数据(原子提交)。 */
static int record_alarm_local(const AlarmTask &task)
{
    if (task.img_draw.empty())
        return 0;
    const std::string &dir = get_store_dir();

    /* 唯一文件名: ch{cam}_{YYYYMMDD-HHMMSS}-{ms}_{seq} */
    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct tm tmv;
    time_t sec = tv.tv_sec;
    localtime_r(&sec, &tmv);
    char stamp[24];
    strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tmv);
    static unsigned long g_seq = 0; /* 仅上传线程访问, 无需加锁 */
    unsigned long seq = ++g_seq;
    char base[160];
    snprintf(base, sizeof(base), "ch%d_%s-%03ld_%lu",
             task.camera_id, stamp, (long)(tv.tv_usec / 1000), seq);

    std::string path_draw = dir + "/" + base + ".jpg";
    std::string path_raw  = dir + "/" + base + "_raw.jpg";
    std::string path_json = dir + "/" + base + ".json";
    std::string path_tmp  = path_json + ".tmp";

    std::vector<int> jpg_params = {cv::IMWRITE_JPEG_QUALITY, 90};
    if (!cv::imwrite(path_draw, task.img_draw, jpg_params))
    {
        fprintf(stderr, "[alarm_recorder] 写图失败: %s\n", path_draw.c_str());
        return 0;
    }
    bool have_raw = !task.img_raw.empty() && cv::imwrite(path_raw, task.img_raw, jpg_params);

    cJSON *root = cJSON_CreateObject();
    if (!root)
    {
        ::unlink(path_draw.c_str());
        if (have_raw) ::unlink(path_raw.c_str());
        return 0;
    }
    cJSON_AddNumberToObject(root, "camera_id", task.camera_id);
    cJSON_AddStringToObject(root, "alarm_type", task.alarm_type.c_str());
    cJSON_AddStringToObject(root, "snapTime", task.snap_time.c_str());
    cJSON_AddStringToObject(root, "endTime", task.snap_time.c_str());
    cJSON_AddStringToObject(root, "server_url", task.server_url.c_str()); /* 方案2: 地址随记录走 */
    cJSON_AddStringToObject(root, "img", (std::string(base) + ".jpg").c_str());
    cJSON_AddStringToObject(root, "img_raw", have_raw ? (std::string(base) + "_raw.jpg").c_str() : "");
    cJSON_AddNumberToObject(root, "ts", (double)time(NULL));

    char *json_str = cJSON_PrintUnformatted(root);
    int ok = 0;
    if (json_str)
    {
        FILE *fp = fopen(path_tmp.c_str(), "wb");
        if (fp)
        {
            fwrite(json_str, 1, strlen(json_str), fp);
            fclose(fp);
            if (rename(path_tmp.c_str(), path_json.c_str()) == 0) /* 原子提交: .json 出现即代表整条记录就绪 */
                ok = 1;
            else
                ::unlink(path_tmp.c_str());
        }
        cJSON_free(json_str);
    }
    cJSON_Delete(root);

    if (!ok)
    {
        ::unlink(path_draw.c_str());
        if (have_raw) ::unlink(path_raw.c_str());
        fprintf(stderr, "[alarm_recorder] 写元数据失败, 已回滚: %s\n", base);
        return 0;
    }

    enforce_store_cap(dir);
    printf("[alarm_recorder] 已存本地告警 ch%d type=%s -> %s.jpg\n",
           task.camera_id, task.alarm_type.c_str(), base);
    return 1;
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
            if (!record_alarm_local(alarm_task))
                fprintf(stderr, "[alarm_recorder] WARNING: 落盘告警失败 (camera %d)\n",
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
