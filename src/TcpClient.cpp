#include "net_sdk/TcpClient.h"
#include "net_sdk/Logger.h"
#include "libhv/hloop.h"
#include <cstring>
#include <thread>
#include <map>
#include <vector>

namespace net_sdk {

// ============================================================================
// TcpClient::State —— 所有状态与 libhv 回调逻辑集中于此，由 shared_ptr 保活。
// 生命周期：TcpClient 持有 state_；每个投递到 libhv 的 token 是 LifeToken 的
// 堆副本，回调据此判断宿主是否仍存活。
//
// 请求级通道约定：pendings / latestByKey 只在 loop 线程读写；业务线程通过
// postToLoop 投递 register/cancel/clear 任务。pendingCount/maxPending 为原子量，
// 供业务线程做背压判断。
// ============================================================================
struct TcpClient::State {
    TcpClientConfig config;
    std::atomic<bool> connected{false};
    std::atomic<int> connId{-1};
    TcpMessageCallback messageCallback;
    TcpConnectionCallback connectionCallback;
    TcpErrorCallback errorCallback;
    EventLoopPool* eventLoopPool = nullptr;

    std::atomic<bool> heartbeatEnabled{false};
    std::string heartbeatData;
    std::atomic<int> heartbeatIntervalMs{0};
    std::atomic<bool> autoReconnect{false};
    std::atomic<int> reconnectIntervalMs{3000};
    std::atomic<bool> reconnecting{false};

    void* conn = nullptr;        // hio_t* —— 仅可在 loop 线程访问
    void* loop = nullptr;        // hloop_t*（共享池或自建）
    void* heartbeatTimer = nullptr;
    void* reconnectTimer = nullptr;

    bool ownsLoop = false;       // 是否自建 loop（无池时）
    std::unique_ptr<std::thread> ownThread;

    std::mutex cbMutex;          // 保护回调 setter/getter、config、frameDecoder 的并发访问
    std::mutex opMutex;          // 保护 connect/disconnect 的状态切换
    TcpFrameDecoder frameDecoder; // 帧解码器（受 cbMutex 保护）
    std::string recvBuffer;      // 未成帧的累积数据（仅 loop 线程访问）

    // ---- 请求级通道数据结构（仅 loop 线程访问）----
    struct TimerCtx {
        State* s;
        uint64_t id;
    };
    struct Pending {
        uint64_t id = 0;
        std::string key;
        TcpResponseCallback cb;
        TcpResponseMatcher matcher;
        void* timer = nullptr;      // htimer_t*
        TimerCtx* tctx = nullptr;   // 定时器 userdata，由 finishPendingOnLoop 释放
        std::atomic<bool> done{false};
    };
    std::map<uint64_t, std::shared_ptr<Pending>> pendings;  // 有序 => FIFO
    std::map<std::string, uint64_t> latestByKey;
    std::atomic<uint64_t> nextRequestId{1};
    std::atomic<int> pendingCount{0};
    std::atomic<int> maxPending{256};
    std::atomic<int> reconnectAttempts{0};

    static const char* errorName(NetErrorCode code) {
        switch (code) {
            case NetErrorCode::Ok:                return "ok";
            case NetErrorCode::Timeout:           return "request timeout";
            case NetErrorCode::Superseded:        return "superseded by newer request";
            case NetErrorCode::Canceled:          return "request canceled";
            case NetErrorCode::ConnectionClosed:  return "connection closed before response";
            case NetErrorCode::SendFailed:        return "send failed";
            case NetErrorCode::ConnectFailed:     return "connect failed";
            default:                              return "unknown error";
        }
    }

    // 仅 loop 线程：结束一个请求，保证「恰好一次回调」。
    // notify=false 用于对象析构 / 宿主已失效时静默回收，不触发业务回调。
    void finishPendingOnLoop(const std::shared_ptr<Pending>& p, NetErrorCode code,
                             const std::string& body, bool notify = true) {
        // 关键：调用方可能直接传 pendings 里元素的引用（cancelRequest / 覆盖旧请求）。
        // 若先 erase 节点，p 会变成悬空引用，后续读 p->cb 即 UAF（回调会静默丢失）。
        // 因此先持有一份局部拷贝，整个函数体只使用 hold。
        std::shared_ptr<Pending> hold = p;
        if (!hold) return;
        if (hold->done.exchange(true)) return;
        if (hold->timer) {
            htimer_del(static_cast<htimer_t*>(hold->timer));
            hold->timer = nullptr;
        }
        if (hold->tctx) {
            // 定时器尚未触发，libhv 删除后不会再回调，userdata 由我们释放。
            delete hold->tctx;
            hold->tctx = nullptr;
        }
        pendings.erase(hold->id);
        if (!hold->key.empty()) {
            auto it = latestByKey.find(hold->key);
            if (it != latestByKey.end() && it->second == hold->id) latestByKey.erase(it);
        }
        pendingCount.fetch_sub(1);
        if (notify && hold->cb) {
            NetResult r;
            r.code = code;
            r.message = errorName(code);
            r.body = body;
            try { hold->cb(r); }
            catch (...) { NET_LOG_ERROR("TcpClient", "response callback threw an exception"); }
        }
    }

    // 仅 loop 线程：批量结束所有在途请求。
    void clearAllPendingOnLoop(bool notify, NetErrorCode code) {
        std::vector<std::shared_ptr<Pending>> list;
        list.reserve(pendings.size());
        for (auto& kv : pendings) list.push_back(kv.second);
        for (auto& p : list) finishPendingOnLoop(p, code, "", notify);
        pendings.clear();
        latestByKey.clear();
    }

    // 仅 loop 线程：为请求挂一次性 watchdog。
    void armTimerOnLoop(const std::shared_ptr<Pending>& p, int timeoutMs) {
        hloop_t* lp = static_cast<hloop_t*>(loop);
        if (!lp) return;
        auto* ctx = new TimerCtx{this, p->id};
        htimer_t* t = htimer_add(lp, [](htimer_t* timer) {
            auto* c = static_cast<TimerCtx*>(hevent_userdata(timer));
            if (!c) return;
            State* s = c->s;
            uint64_t id = c->id;
            hevent_set_userdata(timer, nullptr);
            if (!s) { delete c; return; }
            auto life = s->lifeW.lock();
            if (!life || !life->alive.load()) { delete c; return; }
            auto it = s->pendings.find(id);
            if (it == s->pendings.end()) { delete c; return; }
            auto p = it->second;
            p->timer = nullptr;   // 一次性定时器已自然到期，libhv 自行回收
            p->tctx = nullptr;    // ctx 由本回调释放
            s->finishPendingOnLoop(p, NetErrorCode::Timeout, "");
            delete c;
        }, timeoutMs, 1);
        if (!t) { delete ctx; return; }
        hevent_set_userdata(t, ctx);
        p->timer = t;
        p->tctx = ctx;
    }

    // 仅 loop 线程：把收到的一帧配给在途请求。
    // 规则：先按请求发起顺序调用各 matcher（首个返回 true 者命中）；
    //       若无 matcher 命中，再按 FIFO 配给最早的「无 matcher」请求。
    void dispatchResponseOnLoop(const std::string& data) {
        std::shared_ptr<Pending> hit;
        for (auto& kv : pendings) {
            auto& p = kv.second;
            if (p->done.load() || !p->matcher) continue;
            bool matched = false;
            try { matched = p->matcher(data); }
            catch (...) { matched = false; }
            if (matched) { hit = p; break; }
        }
        if (!hit) {
            for (auto& kv : pendings) {
                auto& p = kv.second;
                if (!p->done.load() && !p->matcher) { hit = p; break; }
            }
        }
        if (hit) finishPendingOnLoop(hit, NetErrorCode::Ok, data);
    }

    // 仅 loop 线程：登记请求（含同 key 覆盖）并立即写帧，不等任何旧请求。
    void registerPendingOnLoop(const std::shared_ptr<Pending>& p,
                               const std::string& frame, int timeoutMs) {
        auto life = lifeW.lock();
        if (!life || !life->alive.load()) {
            finishPendingOnLoop(p, NetErrorCode::ConnectionClosed, "", false);
            return;
        }
        if (!p->key.empty()) {
            auto it = latestByKey.find(p->key);
            if (it != latestByKey.end()) {
                auto old = pendings.find(it->second);
                if (old != pendings.end()) {
                    finishPendingOnLoop(old->second, NetErrorCode::Superseded, "");
                }
            }
            latestByKey.erase(p->key);
        }
        pendings[p->id] = p;
        if (!p->key.empty()) latestByKey[p->key] = p->id;
        if (timeoutMs > 0) armTimerOnLoop(p, timeoutMs);
        if (conn && connected.load()) {
            int ret = hio_write(static_cast<hio_t*>(conn), frame.data(), (int)frame.size());
            if (ret < 0) {
                NET_LOG_ERROR("TcpClient", "sendRequest write failed: " + std::to_string(ret));
                finishPendingOnLoop(p, NetErrorCode::SendFailed, "");
            }
        } else {
            finishPendingOnLoop(p, NetErrorCode::ConnectionClosed, "");
        }
    }

    // ---- 回调分发（在 loop 线程执行）----
    // 先按帧解码器拆帧，再对「每一个完整帧」分别做响应配对与消息派发。
    // 不拆分的话，多请求并发时多个回包会被合并进一次 read，matcher 全部失配。
    void onMessage(const std::string& data) {
        NET_LOG_TRACE("TcpClient", "Received " + std::to_string(data.size()) + " bytes");
        TcpFrameDecoder dec;
        { std::lock_guard<std::mutex> lk(cbMutex); dec = frameDecoder; }
        std::vector<std::string> frames;
        if (dec) {
            recvBuffer.append(data);
            std::string frame;
            try {
                while (dec(recvBuffer, frame)) {
                    frames.push_back(frame);
                    frame.clear();
                }
            } catch (...) {
                NET_LOG_ERROR("TcpClient", "frame decoder threw an exception");
                recvBuffer.clear();
            }
        } else {
            frames.push_back(data);
        }
        for (const auto& f : frames) {
            dispatchResponseOnLoop(f);
            TcpMessageCallback cb;
            { std::lock_guard<std::mutex> lk(cbMutex); cb = messageCallback; }
            if (cb) {
                try { cb(f); }
                catch (...) { NET_LOG_ERROR("TcpClient", "message callback threw an exception"); }
            }
        }
    }

    void onConnected() {
        connId.store(1);
        connected.store(true);
        reconnectAttempts.store(0);
        NET_LOG_INFO("TcpClient", "Connected to " + config.host + ":" + std::to_string(config.port));
        TcpConnectionCallback cb;
        { std::lock_guard<std::mutex> lk(cbMutex); cb = connectionCallback; }
        if (cb) cb(connId.load());
        if (heartbeatEnabled.load()) startHeartbeatOnLoop();
    }

    void onDisconnected() {
        connected.store(false);
        recvBuffer.clear();  // 断连后残留的半帧不能带到下一次连接
        // 连接已断：不再让在途请求干等到 watchdog，立即统一回调。
        clearAllPendingOnLoop(true, NetErrorCode::ConnectionClosed);
        NET_LOG_INFO("TcpClient", "Disconnected from " + config.host);
        TcpConnectionCallback cb;
        TcpErrorCallback ecb;
        int maxRetry = 0;
        {
            std::lock_guard<std::mutex> lk(cbMutex);
            cb = connectionCallback;
            ecb = errorCallback;
            maxRetry = config.maxRetryCount;
        }
        if (cb) cb(-1);
        if (ecb) ecb(connId.load(), NetErrorCode::ConnectionClosed, "connection closed");
        if (autoReconnect.load() && !reconnecting.load()) {
            if (maxRetry > 0 && reconnectAttempts.load() >= maxRetry) {
                NET_LOG_WARN("TcpClient", "auto reconnect stopped after "
                    + std::to_string(maxRetry) + " attempts");
                TcpErrorCallback e2;
                { std::lock_guard<std::mutex> lk(cbMutex); e2 = errorCallback; }
                if (e2) e2(connId.load(), NetErrorCode::ConnectFailed, "max reconnect attempts reached");
                return;
            }
            reconnectAttempts.fetch_add(1);
            tryReconnectOnLoop();
        }
    }

    void onError(NetErrorCode code, const std::string& msg) {
        NET_LOG_ERROR("TcpClient", "Error: " + msg);
        TcpErrorCallback cb;
        { std::lock_guard<std::mutex> lk(cbMutex); cb = errorCallback; }
        if (cb) cb(connId.load(), code, msg);
    }

    // ---- 以下以 OnLoop 结尾的方法仅可在 loop 线程调用 ----
    void startHeartbeatOnLoop() {
        if (!heartbeatEnabled.load() || !conn || heartbeatTimer) return;
        hloop_t* lp = static_cast<hloop_t*>(loop);
        if (!lp) return;
        htimer_t* timer = htimer_add(lp, [](htimer_t* t) {
            auto* s = static_cast<State*>(hevent_userdata(t));
            if (!s) return;
            auto life = s->lifeW.lock();
            if (!life || !life->alive.load()) return;
            std::string payload;
            { std::lock_guard<std::mutex> lk(s->cbMutex); payload = s->heartbeatData; }
            if (s->conn && s->connected.load()) {
                hio_write(static_cast<hio_t*>(s->conn), payload.data(), (int)payload.size());
            }
        }, heartbeatIntervalMs.load(), INFINITE);
        hevent_set_userdata(timer, selfPtr);
        heartbeatTimer = timer;
        NET_LOG_DEBUG("TcpClient", "Heartbeat started, interval=" + std::to_string(heartbeatIntervalMs.load()) + "ms");
    }

    void stopHeartbeatOnLoop() {
        if (heartbeatTimer) {
            htimer_del(static_cast<htimer_t*>(heartbeatTimer));
            heartbeatTimer = nullptr;
        }
        if (reconnectTimer) {
            htimer_del(static_cast<htimer_t*>(reconnectTimer));
            reconnectTimer = nullptr;
        }
    }

    void tryReconnectOnLoop() {
        if (reconnecting.load()) return;
        reconnecting.store(true);
        hloop_t* lp = static_cast<hloop_t*>(loop);
        if (!lp) { reconnecting.store(false); return; }
        NET_LOG_INFO("TcpClient", "Auto reconnecting in " + std::to_string(reconnectIntervalMs.load()) + "ms");
        htimer_t* timer = htimer_add(lp, [](htimer_t* t) {
            auto* holder = static_cast<State**>(hevent_userdata(t));
            if (!holder || !*holder) return;
            State* s = *holder;
            auto life = s->lifeW.lock();
            if (!life || !life->alive.load()) return;
            s->reconnecting.store(false);
            s->connectOnLoop(s->config);
        }, reconnectIntervalMs.load(), 1);
        hevent_set_userdata(timer, &reconnectHolder);
        reconnectTimer = timer;
    }

    // 真正建立连接（loop 线程）
    void connectOnLoop(const TcpClientConfig& cfg) {
        hloop_t* lp = static_cast<hloop_t*>(loop);
        if (!lp) return;
        // 复用已有连接对象时先清理
        if (conn) {
            hio_close(static_cast<hio_t*>(conn));
            conn = nullptr;
        }
        hio_t* io = hloop_create_tcp_client(lp, cfg.host.c_str(), cfg.port,
            [](hio_t* c) {
                auto* holder = static_cast<State**>(hio_context(c));
                if (!holder || !*holder) return;
                State* s = *holder;
                auto life = s->lifeW.lock();
                if (!life || !life->alive.load()) return;
                if (hio_error(c) == 0) s->onConnected();
                else s->onError(NetErrorCode::ConnectFailed, "Connect failed");
            });
        if (!io) {
            NET_LOG_ERROR("TcpClient", "Failed to create TCP client: " + cfg.host + ":" + std::to_string(cfg.port));
            return;
        }
        conn = io;
        // context 保存 State*(弱引用宿主)，配合 lifeW 强校验
        hio_set_context(io, &stateHolder);
        hio_setcb_read(io, [](hio_t* c, void* buf, int readbytes) {
            auto* holder = static_cast<State**>(hio_context(c));
            if (!holder || !*holder) return;
            State* s = *holder;
            auto life = s->lifeW.lock();
            if (!life || !life->alive.load()) return;
            s->onMessage(std::string((char*)buf, readbytes));
        });
        hio_setcb_close(io, [](hio_t* c) {
            auto* holder = static_cast<State**>(hio_context(c));
            if (!holder || !*holder) return;
            State* s = *holder;
            auto life = s->lifeW.lock();
            if (!life || !life->alive.load()) return;
            s->conn = nullptr;
            s->onDisconnected();
        });
        hio_set_connect_timeout(io, cfg.connectTimeoutMs);
        hio_set_read_timeout(io, cfg.readTimeoutMs);
        if (cfg.writeTimeoutMs > 0) hio_set_write_timeout(io, cfg.writeTimeoutMs);
        hio_read(io);
        NET_LOG_INFO("TcpClient", "Connecting to " + cfg.host + ":" + std::to_string(cfg.port));
    }

    // 由外部注入：弱令牌 + 供回调回填的 State 指针槽
    std::weak_ptr<LifeToken> lifeW;
    std::shared_ptr<LifeToken> lifeStrong; // 防止 State 使用期间令牌被释放
    State* selfPtr = nullptr;              // 指向自身（State 由 shared_ptr 持有，稳定地址）
    State* stateHolder = nullptr;          // = selfPtr，供 hio context 存地址
    State* reconnectHolder = nullptr;      // = selfPtr，供重连定时器存地址
    void* userdataHolder = nullptr;        // 保留字段
};

// 静态 post_event 辅助：把 lambda 投递到 loop 线程执行。
// 用 shared_ptr 捕获 State 与任务，保证跨线程期间对象存活。
namespace {
struct PostCtx {
    std::function<void()> fn;
};
void postToLoop(void* loop, std::function<void()> fn) {
    if (!loop) return;
    auto* ctx = new PostCtx{std::move(fn)};
    hevent_t ev;
    std::memset(&ev, 0, sizeof(ev));
    ev.cb = [](hevent_t* e) {
        auto* c = static_cast<PostCtx*>(hevent_userdata(e));
        if (c) { c->fn(); delete c; }
    };
    hevent_set_userdata(&ev, ctx);
    hloop_post_event(static_cast<hloop_t*>(loop), &ev);
}
} // namespace

TcpClient::TcpClient()
    : state_(std::make_shared<State>()), life_(std::make_shared<LifeToken>()) {
    state_->selfPtr = state_.get();
    state_->stateHolder = state_.get();
    state_->reconnectHolder = state_.get();
    state_->userdataHolder = state_.get();
    state_->lifeW = life_;
    state_->lifeStrong = life_;
    NET_LOG_DEBUG("TcpClient", "TcpClient created");
}

TcpClient::~TcpClient() {
    // 标记销毁：所有在途 libhv 回调据此安全跳过。
    life_->alive.store(false);
    auto st = state_;
    void* loop = st->loop;
    // 在 loop 线程清理定时器、在途请求与连接，避免跨线程 htimer_del/hio_close。
    postToLoop(loop, [st]() {
        st->stopHeartbeatOnLoop();
        st->clearAllPendingOnLoop(false, NetErrorCode::ConnectionClosed);
        if (st->conn) {
            hio_set_context(static_cast<hio_t*>(st->conn), nullptr);
            hio_close(static_cast<hio_t*>(st->conn));
            st->conn = nullptr;
        }
        st->connected.store(false);
    });
    // 自建 loop：停止并回收线程（post 的事件会在 stop 前被处理）。
    if (st->ownsLoop && st->ownThread) {
        hloop_stop(static_cast<hloop_t*>(st->loop));
        if (st->ownThread->joinable()) st->ownThread->join();
        st->ownThread.reset();
    }
    NET_LOG_DEBUG("TcpClient", "TcpClient destroyed");
}

bool TcpClient::connect(const TcpClientConfig& config) {
    auto st = state_;
    if (!life_->alive.load()) return false;
    std::lock_guard<std::mutex> lk(st->opMutex);
    {
        std::lock_guard<std::mutex> cb(st->cbMutex);
        st->config = config;
    }
    st->maxPending.store(config.maxPendingRequests > 0 ? config.maxPendingRequests : 256);
    // 尚未绑定 loop：此处确定 loop（共享池或自建并启动线程）。
    if (!st->loop) {
        if (st->eventLoopPool) {
            st->loop = st->eventLoopPool->getNextLoop();
        } else {
            hloop_t* lp = hloop_new(HLOOP_FLAG_AUTO_FREE);
            st->loop = lp;
            st->ownsLoop = true;
            st->ownThread = std::make_unique<std::thread>([lp]() { hloop_run(lp); });
        }
        if (!st->loop) {
            NET_LOG_ERROR("TcpClient", "Failed to get event loop");
            return false;
        }
    }
    postToLoop(st->loop, [st, config]() { st->connectOnLoop(config); });
    return true;
}

void TcpClient::disconnect() {
    auto st = state_;
    if (!life_->alive.load()) return;
    std::lock_guard<std::mutex> lk(st->opMutex);
    postToLoop(st->loop, [st]() {
        st->stopHeartbeatOnLoop();
        st->clearAllPendingOnLoop(true, NetErrorCode::ConnectionClosed);
        st->recvBuffer.clear();
        if (st->conn) {
            hio_set_context(static_cast<hio_t*>(st->conn), nullptr);
            hio_close(static_cast<hio_t*>(st->conn));
            st->conn = nullptr;
        }
        st->connected.store(false);
    });
    NET_LOG_INFO("TcpClient", "Disconnect requested");
}

bool TcpClient::isConnected() const {
    return state_->connected.load();
}

int TcpClient::getConnectionId() const {
    return state_->connId.load();
}

bool TcpClient::send(const std::string& data) {
    return send(data.data(), data.size());
}

bool TcpClient::send(const char* data, size_t len) {
    auto st = state_;
    if (!life_->alive.load() || !st->connected.load()) {
        NET_LOG_ERROR("TcpClient", "Not connected, cannot send");
        return false;
    }
    std::string copy(data, len);
    postToLoop(st->loop, [st, copy]() {
        if (!st->conn || !st->connected.load()) return;
        int ret = hio_write(static_cast<hio_t*>(st->conn), copy.data(), (int)copy.size());
        if (ret < 0) NET_LOG_ERROR("TcpClient", "Send failed: " + std::to_string(ret));
    });
    return true;
}

uint64_t TcpClient::sendRequest(const std::string& data, int timeoutMs,
                                const TcpResponseCallback& callback,
                                const TcpResponseMatcher& matcher) {
    return sendRequestImpl("", data, timeoutMs, callback, matcher);
}

uint64_t TcpClient::sendRequestLatest(const std::string& key, const std::string& data,
                                      int timeoutMs, const TcpResponseCallback& callback,
                                      const TcpResponseMatcher& matcher) {
    return sendRequestImpl(key, data, timeoutMs, callback, matcher);
}

uint64_t TcpClient::sendRequestImpl(const std::string& key, const std::string& data,
                                    int timeoutMs, const TcpResponseCallback& callback,
                                    const TcpResponseMatcher& matcher) {
    auto st = state_;
    if (!life_->alive.load() || !st->loop || !st->connected.load()) {
        NET_LOG_WARN("TcpClient", "sendRequest: not connected");
        return 0;
    }
    int maxP = st->maxPending.load();
    if (maxP > 0 && st->pendingCount.load() >= maxP) {
        NET_LOG_WARN("TcpClient", "sendRequest: pending request limit reached (backpressure)");
        return 0;
    }
    auto p = std::make_shared<TcpClient::State::Pending>();
    p->id = st->nextRequestId.fetch_add(1);
    p->key = key;
    p->cb = callback;
    p->matcher = matcher;
    st->pendingCount.fetch_add(1);
    postToLoop(st->loop, [st, p, data, timeoutMs]() {
        st->registerPendingOnLoop(p, data, timeoutMs);
    });
    return p->id;
}

bool TcpClient::cancelRequest(uint64_t requestId) {
    auto st = state_;
    if (!life_->alive.load() || !st->loop) return false;
    postToLoop(st->loop, [st, requestId]() {
        auto it = st->pendings.find(requestId);
        if (it != st->pendings.end())
            st->finishPendingOnLoop(it->second, NetErrorCode::Canceled, "");
    });
    return true;
}

void TcpClient::cancelAllRequests() {
    auto st = state_;
    if (!life_->alive.load() || !st->loop) return;
    postToLoop(st->loop, [st]() {
        st->clearAllPendingOnLoop(true, NetErrorCode::Canceled);
    });
}

int TcpClient::getPendingRequestCount() const {
    return state_->pendingCount.load();
}

void TcpClient::setMaxPendingRequests(int n) {
    state_->maxPending.store(n > 0 ? n : 256);
}

void TcpClient::setFrameDecoder(const TcpFrameDecoder& decoder) {
    std::lock_guard<std::mutex> lk(state_->cbMutex);
    state_->frameDecoder = decoder;
    state_->recvBuffer.clear();
}

void TcpClient::setLineFrameMode(char delimiter) {
    setFrameDecoder([delimiter](std::string& buf, std::string& frame) {
        size_t pos = buf.find(delimiter);
        if (pos == std::string::npos) return false;
        frame.assign(buf, 0, pos);
        buf.erase(0, pos + 1);
        return true;
    });
}

void TcpClient::setMessageCallback(TcpMessageCallback callback) {
    std::lock_guard<std::mutex> lk(state_->cbMutex);
    state_->messageCallback = std::move(callback);
}

void TcpClient::setConnectionCallback(TcpConnectionCallback callback) {
    std::lock_guard<std::mutex> lk(state_->cbMutex);
    state_->connectionCallback = std::move(callback);
}

void TcpClient::setErrorCallback(TcpErrorCallback callback) {
    std::lock_guard<std::mutex> lk(state_->cbMutex);
    state_->errorCallback = std::move(callback);
}

void TcpClient::setEventLoopPool(EventLoopPool* pool) {
    std::lock_guard<std::mutex> lk(state_->opMutex);
    state_->eventLoopPool = pool;
}

void TcpClient::enableHeartbeat(const std::string& heartbeatData, int intervalMs) {
    auto st = state_;
    {
        std::lock_guard<std::mutex> lk(st->cbMutex);
        st->heartbeatData = heartbeatData;
    }
    st->heartbeatIntervalMs.store(intervalMs);
    st->heartbeatEnabled.store(true);
    postToLoop(st->loop, [st]() { st->startHeartbeatOnLoop(); });
}

void TcpClient::disableHeartbeat() {
    auto st = state_;
    st->heartbeatEnabled.store(false);
    postToLoop(st->loop, [st]() { st->stopHeartbeatOnLoop(); });
}

void TcpClient::setAutoReconnect(bool enable, int intervalMs) {
    state_->autoReconnect.store(enable);
    state_->reconnectIntervalMs.store(intervalMs);
    if (enable) state_->reconnectAttempts.store(0);
}

} // namespace net_sdk
