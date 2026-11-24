/**
 * @file Common.h
 * @brief NetSDK 公共类型定义
 *
 * 定义了 NetSDK 所有模块共用的基础类型，包括错误码、请求/响应结构体、
 * 回调函数类型、配置结构体等。这些类型是 SDK 各模块之间通信的基础契约。
 *
 * @author NetSDK Team
 * @version 1.0.0
 * @date 2026-11-18
 */

#pragma once

#include <string>
#include <functional>
#include <memory>
#include <chrono>
#include <atomic>
#include <cstdint>
#include <map>
#include <vector>

namespace net_sdk {

/**
 * @brief 网络错误码枚举
 *
 * 定义了 SDK 中所有可能出现的错误类型，用于在 NetResult 和回调中标识错误原因。
 */
enum class NetErrorCode {
    Ok = 0,                ///< 操作成功
    ConnectFailed = 1,     ///< 连接失败
    SendFailed = 2,        ///< 发送数据失败
    ReceiveFailed = 3,     ///< 接收数据失败
    Timeout = 4,           ///< 操作超时
    InvalidUrl = 5,        ///< URL 格式无效
    InvalidParam = 6,      ///< 参数无效
    DnsResolveFailed = 7,  ///< DNS 解析失败
    SslHandshakeFailed = 8,///< SSL 握手失败
    ConnectionClosed = 9,  ///< 连接已关闭
    CommandTimeout = 10,   ///< 命令请求超时
    CommandFailed = 11,    ///< 命令请求失败
    JsonParseError = 12,   ///< JSON 解析错误
    SqliteError = 13,      ///< SQLite 数据库错误
    Superseded = 14,       ///< 同 key 的更新请求到来，本请求被取代（旧请求立即回调，不再等待网络返回）
    Canceled = 15,         ///< 请求被显式取消
    UnknownError = 99      ///< 未知错误
};

/**
 * @brief 网络请求结果结构体
 *
 * 用于封装 HTTP 请求或 TCP 操作的返回结果，包含状态码、响应体、头部信息等。
 */
struct NetResult {
    NetErrorCode code = NetErrorCode::Ok;              ///< 错误码
    std::string message;                                ///< 错误或状态描述信息
    int statusCode = 0;                                 ///< HTTP 状态码（仅 HTTP 请求有效）
    std::string body;                                   ///< 响应体内容
    std::map<std::string, std::string> headers;         ///< 响应头部键值对

    bool ok() const { return code == NetErrorCode::Ok; }   ///< 判断是否成功
    operator bool() const { return ok(); }                  ///< 隐式转换为 bool
};

/// HTTP 异步请求回调函数类型
using NetCallback = std::function<void(const NetResult&)>;

/**
 * @brief 宿主回调投递器（v1.3.0 起，解决「回调不在宿主 UI 主线程」的根因）
 *
 * SDK 的异步回调默认由内部线程池 / dispatcher 泵线程执行，并不等于宿主 UI
 * 主线程。宿主通过 setCallbackPoster() 注入一个「把任务投递到指定线程」的函数，
 * SDK 的所有异步结果回调（含 requestAsyncTyped 的模板解析结果）都会经它投递，
 * 业务层因此无需、也不应再自行做二次线程切换。
 *
 * 契约（务必遵守）：
 * - 会被 SDK 的任意工作线程并发调用，实现必须线程安全；
 * - 必须保证 task 最终被执行且仅执行一次，不可静默丢弃
 *   （不要用非线程安全的 WeakPtrFactory / ScopedRunnableMethodFactory）；
 * - 若宿主消息循环尚未就绪，应就地执行 task 兜底，避免回调永久丢失；
 * - 传空函数可取消注入，回到 SDK 默认（dispatcher / 直接调用）行为。
 *
 * 示例（UI 应用）：
 * @code
 *   net_sdk::NetSdk::getInstance().setCallbackPoster([](std::function<void()> task) {
 *       // MessageLoop::PostTask 可从任意线程调用，且 NewRunnableFunction
 *       // 不依赖非线程安全的 WeakPtrFactory。
 *       if (Util::IsInitAsyncMsgLoop() && Util::GetMainThreadLoop())
 *           Util::GetMainThreadLoop()->PostTask(async_task::NewRunnableFunction(std::move(task)));
 *       else
 *           task();
 *   });
 * @endcode
 */
using CallbackPoster = std::function<void(std::function<void()> task)>;

/**
 * @brief HTTP 请求配置结构体
 *
 * 用于配置 HTTP 请求的各项参数，包括 URL、方法、头部、超时等。
 */
struct HttpRequestConfig {
    std::string url;                            ///< 请求 URL
    std::string method = "GET";                 ///< HTTP 方法（GET/POST/PUT/DELETE 等）
    std::map<std::string, std::string> headers; ///< 请求头部键值对
    std::string body;                           ///< 请求体内容
    int timeoutMs = 30000;                      ///< 超时时间（毫秒）
    bool followRedirect = true;                 ///< 是否自动跟随重定向（已生效）
    int maxRedirect = 5;                        ///< 最大重定向次数（底层 libhv 使用其内部默认上限，此值作为声明保留）
    bool verifySsl = true;                      ///< 是否验证 SSL 证书（需带 SSL 的 libhv，见说明）
    std::string caPath;                         ///< CA 证书路径（需带 SSL 的 libhv）
    std::string certPath;                       ///< 客户端证书路径（需带 SSL 的 libhv）
    std::string keyPath;                        ///< 客户端私钥路径（需带 SSL 的 libhv）
    // 说明：随 SDK 发布的 libhv 已用 OpenSSL 1.1.1 重新编译（WITH_OPENSSL），
    // HTTP/HTTPS 均可直接使用。若需开启对端证书校验，请在首次 HTTPS 请求之前调用
    // HttpClient::configureSsl(verify, caFile, caPath) 配置全局 SSL_CTX。
};

/**
 * @brief TCP 客户端配置结构体
 *
 * 用于配置 TCP 客户端连接参数，包括主机、端口、超时、心跳、自动重连等。
 */
struct TcpClientConfig {
    std::string host;                    ///< 目标主机地址
    int port = 0;                        ///< 目标端口
    int connectTimeoutMs = 10000;        ///< 连接超时时间（毫秒）
    int readTimeoutMs = 30000;           ///< 读空闲超时（毫秒）；建议大于 sendRequest 的 timeoutMs
    int writeTimeoutMs = 30000;          ///< 写超时（毫秒，0 表示不启用）
    bool keepAlive = true;               ///< 保留字段：长连接（当前实现始终为长连接）
    bool autoReconnect = false;          ///< 是否启用自动重连
    int reconnectIntervalMs = 3000;      ///< 重连间隔时间（毫秒）
    int heartbeatIntervalMs = 0;         ///< 心跳间隔时间（毫秒，0 表示不发送心跳）
    int maxRetryCount = 3;               ///< 最大连续重连次数（0 表示不限制；连接成功后清零）
    int maxPendingRequests = 256;        ///< 在途请求上限（背压）；达到上限时 sendRequest 返回 0
};

/**
 * @brief TCP 服务端配置结构体
 *
 * 用于配置 TCP 服务端监听参数，包括监听地址、端口、线程数等。
 */
struct TcpServerConfig {
    std::string host = "0.0.0.0";        ///< 监听地址
    int port = 0;                        ///< 监听端口
    int threadNum = 1;                   ///< IO 工作线程数（1 = acceptor 独占单 loop；>1 启用多 worker loop 并发）
    int readTimeoutMs = 60000;           ///< 读空闲超时（毫秒，0 表示不启用）
    bool enableHeartbeat = false;        ///< 是否用心跳周期作为读空闲检测周期
    int heartbeatIntervalMs = 30;        ///< 心跳间隔时间（秒，enableHeartbeat=true 时覆盖 readTimeoutMs）
};

/// TCP 消息接收回调函数类型（原始流式通道）
using TcpMessageCallback = std::function<void(const std::string& data)>;
/// TCP 连接状态变化回调函数类型（connId > 0 表示连接，connId < 0 表示断开）
using TcpConnectionCallback = std::function<void(int connId)>;
/// TCP 错误回调函数类型
using TcpErrorCallback = std::function<void(int connId, NetErrorCode code, const std::string& msg)>;

/**
 * @brief TCP 请求级响应回调
 *
 * 由 sendRequest()/sendRequestLatest() 使用，在事件循环线程执行；
 * 如需更新 UI 请通过 MainThreadDispatcher 切换。
 * - code == Ok           : body 为匹配到的回包数据
 * - code == Timeout      : 请求级 watchdog 超时，迟到回包会被丢弃
 * - code == Superseded   : 同 key 的更新请求到来，本请求被取代
 * - code == Canceled     : 被 cancelRequest()/cancelAllRequests() 取消
 * - code == ConnectionClosed : 连接在回包前断开
 */
using TcpResponseCallback = std::function<void(const NetResult&)>;

/**
 * @brief 可选的响应匹配器
 *
 * TCP 是流式协议，SDK 默认按 FIFO（先发先收）把收到的帧配给最早的在途请求。
 * 若业务协议带请求 ID / 序列号，可提供 matcher 精确匹配：
 * 收到一帧时按请求发起顺序调用各 matcher，首个返回 true 的请求获得该帧。
 * 为空（默认）时该请求走 FIFO 配对。
 */
using TcpResponseMatcher = std::function<bool(const std::string& frame)>;

/**
 * @brief TCP 帧解码器（解决粘包 / 半包）
 *
 * TCP 是字节流，一次 read 可能包含多个业务帧，也可能只包含半个帧。
 * 未设置解码器时，SDK 默认把每次 read 的数据当作一帧。
 *
 * 解码器契约：
 * - 从累积接收缓冲 buffer 中尝试解析出一帧；
 * - 成功：把该帧写入 frame，从 buffer 中移除已消费字节，返回 true
 *   （SDK 会循环调用，直到返回 false）；
 * - 数据不足：保持 buffer 不变，返回 false。
 *
 * 可直接用 TcpClient::setLineFrameMode() / TcpServer::setLineFrameMode()
 * 启用按分隔符分帧，无需自己实现。
 */
using TcpFrameDecoder = std::function<bool(std::string& buffer, std::string& frame)>;

/**
 * @brief 通用命令请求配置结构体
 *
 * 用于配置基于 cmd/cmdId 的通用命令请求，支持超时、请求 ID 等参数。
 * 适用于业务层通过统一接口发送不同类型的命令请求。
 */
struct CommandConfig {
    int cmdId = 0;                       ///< 命令 ID（业务自定义）
    std::string cmd;                     ///< 命令名称
    int timeoutMs = 30000;               ///< 超时时间（毫秒）
    std::string requestId;               ///< 请求唯一标识（自动生成）
    std::map<std::string, std::string> params; ///< 请求参数键值对
    std::string body;                    ///< 请求体内容（JSON 格式）

    CommandConfig() = default;
    CommandConfig(int id, const std::string& c) : cmdId(id), cmd(c) {}
};

/**
 * @brief 通用命令请求结果结构体
 *
 * 封装命令请求的响应数据，包含请求 ID、命令 ID、业务码、原始数据等。
 */
struct CommandResult {
    std::string requestId;               ///< 请求唯一标识
    int cmdId = 0;                       ///< 命令 ID
    int code = 0;                        ///< 业务返回码（0 表示成功）
    std::string message;                 ///< 业务返回消息
    std::string rawData;                 ///< 原始响应数据（JSON 格式字符串）

    bool ok() const { return code == 0; }    ///< 判断是否成功
    operator bool() const { return ok(); }    ///< 隐式转换为 bool
};

/// 命令请求回调函数类型（回调在主线程执行）
using CommandCallback = std::function<void(const CommandResult&)>;

} // namespace net_sdk