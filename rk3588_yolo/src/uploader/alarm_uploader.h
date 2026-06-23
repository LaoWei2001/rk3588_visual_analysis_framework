#pragma once
#include <opencv2/core/core.hpp>
#include <string>

#ifdef __cplusplus
extern "C"
{
#endif

    /* 初始化 Redis 连接 + 内部异步上传线程 (pthread) */
    int alarm_uploader_init(const char *redis_host, int redis_port);

    /* 反初始化: 停止上传线程 + 释放 Redis 连接 */
    void alarm_uploader_deinit(void);

    /* 异步推送到服务器队列 (非阻塞)
     * report_enable: 是否上报 —— 画布上连了"上报配置"节点才为 true(取
     * ctx->config->report_enable, 可用 report_enabled(ctx) 取); 传 false
     * 时本函数直接跳过、不入队, 让该节点成为真正的开关。
     * server_url: 该通道的 HTTP 上报地址, 随消息进 Redis (方案2); 传 nullptr/空 =
     * 用上报服务默认值 */
    int alarm_uploader_enqueue(const cv::Mat &img_draw, const cv::Mat &img_raw, int camera_id, const char *alarm_type,
                               bool report_enable, const char *server_url = nullptr);

    /* 异步推送到 Dify 队列 (非阻塞)
     * report_enable: 同上, false = 不上报直接跳过。
     * dify_api_url / dify_api_key: 该通道的 Dify 地址与密钥, 随消息进 Redis
     * (方案2); 传 nullptr/空 = 用默认值 */
    int dify_uploader_enqueue(const cv::Mat &img, const char *prompt, const char *event_id, bool report_enable,
                              const char *dify_api_url = nullptr, const char *dify_api_key = nullptr);

#ifdef __cplusplus
}
#endif
