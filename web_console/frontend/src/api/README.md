# API 客户端

`http.ts` 是公共 Axios 入口，统一登录请求头、过期登录处理、错误信息转换及在途读取合并。
各业务模块只维护自己的请求和数据类型；新调用优先直接从对应模块导入。
`client.ts` 保留原有导出的兼容入口，不再添加请求实现。

`auth`、`apps`、`logics`、`playback`、`delivery`、`records` 对应应用运行与业务数据；
`system`、`recovery`、`device`、`storage`、`network`、`cameras`、`services` 对应设备管理。
`dataset` 与 `videoCapture` 保持各自业务模块，复用公共传输。

网络切换期间跨地址探测仍使用独立 Axios 请求，避免探测地址的失败影响当前登录。
图片与媒体 URL 使用各模块原有鉴权方式；Blob 下载错误的专用解析保留在数据集模块内。
