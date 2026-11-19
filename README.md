# net_sdk
NetSDK 是一个面向生产环境的 **C++17 跨平台网络 SDK**，底层基于 [libhv](https://github.com/ithewei/libhv)。 目标：让大小型项目**只传参数、拿结果**——异步、并发、超时、重试、JSON 解析、日志、线程切换全部在 SDK 底层完成，业务层回调**直接落在自己的 UI 主线程**。
