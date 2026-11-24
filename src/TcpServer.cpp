#include "net_sdk/TcpServer.h"
#include "net_sdk/Logger.h"
#include "libhv/hloop.h"
#include <cstring>
#include <thread>
#include <vector>
#include <map>

namespace net_sdk {

// 每连接上下文：以「值」存进 State::connections（std::map 节点地址稳定），
// hio_context 直接指向该节点，避免 new/delete 与跨 loop 释放问题。
struct TcpServerConnCtx {
    void* state = nullptr;   // TcpServer::State*
    int connId = 0;
    void* loop = nullptr;    // 该连接所属的 hloop_t*（send/close 需投递到同一 loop）
    void* io = nullptr;      // hio_t*
    std::string recvBuffer;  // 该连接未成帧的累积数据（仅所属 loop 线程访问）
};

struct TcpServer::State {
    TcpServerConfig config;
    std::atomic<bool> running{false};
    TcpMessageCallback messageCallback;
    TcpConnectionCallback connectionCallback;
    TcpConnectionCallback disconnectionCallback;
    TcpErrorCallback errorCallback;

    // connId -> 上下文（值语义，地址稳定；所有跨 loop 访问由 connectionsMutex 保护）
    std::map<int, TcpServerConnCtx> connections;
    mutable std::mutex connectionsMutex;

    EventLoopPool* eventLoopPool = nullptr;
    void* listenIo = nullptr;
    void* loop = nullptr;                       // acceptor loop
    std::vector<void*> workerLoops;             // threadNum > 1 时的 IO worker loops
    std::unique_ptr<std::thread> ownThread;     // acceptor 线程
    std::vector<std::unique_ptr<std::thread>> workerThreads;
    std::atomic<size_t> nextWorker{0};

    std::mutex cbMutex;
    std::mutex opMutex;
    std::atomic<int> nextConnId{1};
    TcpFrameDecoder frameDecoder;  // 受 cbMutex 保护；每连接缓冲保存在 ConnCtx

    std::weak_ptr<LifeToken> lifeW;
    State* selfPtr = nullptr;

    TcpMessageCallback getMessageCb() {
        std::lock_guard<std::mutex> lk(cbMutex); return messageCallback;
    }
    TcpConnectionCallback getConnCb() {
        std::lock_guard<std::mutex> lk(cbMutex); return connectionCallback;
    }
    TcpConnectionCallback getDiscCb() {
        std::lock_guard<std::mutex> lk(cbMutex); return disconnectionCallback;
    }

    bool alive() const {
        auto l = lifeW.lock();
        return l && l->alive.load();
    }

    int connReadTimeoutMs() {
        std::lock_guard<std::mutex> lk(cbMutex);
        int rt = config.readTimeoutMs;
        if (config.enableHeartbeat && config.heartbeatIntervalMs > 0) {
            rt = config.heartbeatIntervalMs * 1000;
        }
        return rt > 0 ? rt : 0;
    }

    // ---- 以下仅在该连接所属的 loop 线程调用 ----

    // 完成连接的回调/超时设置并启动读。单 loop 时直接在 acceptor loop 调用；
    // 多 worker 时在 hio_attach 之后于 worker loop 调用。
    void setupConnOnLoop(hio_t* io, int connId) {
        TcpServerConnCtx* cc = nullptr;
        {
            std::lock_guard<std::mutex> lk(connectionsMutex);
            auto it = connections.find(connId);
            if (it == connections.end()) { hio_close(io); return; }
            cc = &it->second;
        }
        hio_set_context(io, cc);
        hio_setcb_read(io, [](hio_t* c, void* buf, int readbytes) {
            auto* ctx = static_cast<TcpServerConnCtx*>(hio_context(c));
            if (!ctx || !ctx->state) return;
            auto* s = static_cast<State*>(ctx->state);
            if (!s->alive()) return;
            TcpFrameDecoder dec;
            { std::lock_guard<std::mutex> lk(s->cbMutex); dec = s->frameDecoder; }
            std::vector<std::string> frames;
            if (dec) {
                ctx->recvBuffer.append(static_cast<char*>(buf), static_cast<size_t>(readbytes));
                std::string frame;
                try {
                    while (dec(ctx->recvBuffer, frame)) {
                        frames.push_back(frame);
                        frame.clear();
                    }
                } catch (...) {
                    NET_LOG_ERROR("TcpServer", "frame decoder threw an exception");
                    ctx->recvBuffer.clear();
                }
            } else {
                frames.emplace_back(static_cast<char*>(buf), static_cast<size_t>(readbytes));
            }
            auto cb = s->getMessageCb();
            if (!cb) return;
            for (const auto& f : frames) {
                try { cb(f); }
                catch (...) { NET_LOG_ERROR("TcpServer", "message callback threw an exception"); }
            }
        });
        hio_setcb_close(io, [](hio_t* c) {
            auto* ctx = static_cast<TcpServerConnCtx*>(hio_context(c));
            if (!ctx || !ctx->state) return;
            auto* s = static_cast<State*>(ctx->state);
            int cid = ctx->connId;
            // 先摘除 map 节点（此后 ctx 失效，后续只用已复制的 cid/s）。
            {
                std::lock_guard<std::mutex> lk(s->connectionsMutex);
                s->connections.erase(cid);
            }
            hio_set_context(c, nullptr);
            if (!s->alive()) return;
            NET_LOG_INFO("TcpServer", "Connection closed: connId=" + std::to_string(cid));
            auto cb = s->getDiscCb();
            if (cb) cb(cid);
        });
        int rt = connReadTimeoutMs();
        if (rt > 0) hio_set_read_timeout(io, rt);
        hio_read(io);
    }

    struct AttachCtx {
        void* state;
        void* io;
        int connId;
    };
    static void onAttachEvent(hevent_t* e) {
        auto* c = static_cast<AttachCtx*>(hevent_userdata(e));
        if (!c) return;
        State* s = static_cast<State*>(c->state);
        hio_t* io = static_cast<hio_t*>(c->io);
        int connId = c->connId;
        delete c;
        hevent_set_userdata(e, nullptr);
        if (!s || !s->alive()) { hio_close(io); return; }
        hio_attach(e->loop, io);
        s->setupConnOnLoop(io, connId);
    }

    // acceptor loop 线程：分配 connId / 选 worker loop / 分发连接。
    void acceptOnLoop(hio_t* io) {
        int connId = nextConnId.fetch_add(1);
        void* acceptorLoop = loop;
        void* target = acceptorLoop;
        if (!workerLoops.empty()) {
            size_t idx = nextWorker.fetch_add(1) % workerLoops.size();
            target = workerLoops[idx];
        }
        {
            std::lock_guard<std::mutex> lk(connectionsMutex);
            auto& e = connections[connId];
            e.state = selfPtr;
            e.connId = connId;
            e.loop = target;
            e.io = io;
        }
        NET_LOG_INFO("TcpServer", "New connection: connId=" + std::to_string(connId));
        auto cb = getConnCb();
        if (cb) cb(connId);

        if (target != acceptorLoop) {
            // one-acceptor-multi-workers：从 acceptor loop 摘下，投递到 worker loop。
            hio_detach(io);
            auto* ctx = new AttachCtx{selfPtr, io, connId};
            hevent_t ev;
            std::memset(&ev, 0, sizeof(ev));
            ev.loop = static_cast<hloop_t*>(target);
            ev.cb = &State::onAttachEvent;
            hevent_set_userdata(&ev, ctx);
            hloop_post_event(static_cast<hloop_t*>(target), &ev);
        } else {
            setupConnOnLoop(io, connId);
        }
    }

    // accept 静态 C 回调：acceptor loop 独占 userdata，不会与其它 server 冲突。
    static void onAccept(hio_t* io) {
        hloop_t* lp = static_cast<hloop_t*>(hevent_loop(io));
        if (!lp) return;
        auto* s = static_cast<State*>(hloop_userdata(lp));
        if (!s || !s->alive()) { hio_close(io); return; }
        s->acceptOnLoop(io);
    }
};

namespace {
struct PostCtx { std::function<void()> fn; };
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

TcpServer::TcpServer()
    : state_(std::make_shared<State>()), life_(std::make_shared<LifeToken>()) {
    state_->selfPtr = state_.get();
    state_->lifeW = life_;
    NET_LOG_DEBUG("TcpServer", "TcpServer created");
}

TcpServer::~TcpServer() {
    // 标记销毁：在途回调据此安全跳过，不再触发业务回调。
    life_->alive.store(false);
    stop();
    NET_LOG_DEBUG("TcpServer", "TcpServer destroyed");
}

// 停止全部 loop 并回收线程：先 join 再清空连接表，确保没有回调仍在访问它。
bool TcpServer::start(const TcpServerConfig& config) {
    auto st = state_;
    if (!life_->alive.load()) return false;
    std::lock_guard<std::mutex> lk(st->opMutex);
    if (st->running.load()) return true;
    {
        std::lock_guard<std::mutex> cb(st->cbMutex);
        st->config = config;
    }

    // TcpServer 的 accept 回调依赖 hloop_userdata 传递上下文，因此 acceptor loop
    // 必须独占；EventLoopPool 不适用于 accept，保留 API 仅作兼容。
    if (st->eventLoopPool) {
        NET_LOG_WARN("TcpServer", "EventLoopPool ignored: TcpServer uses a dedicated acceptor loop");
    }

    hloop_t* lp = hloop_new(HLOOP_FLAG_AUTO_FREE);
    if (!lp) {
        NET_LOG_ERROR("TcpServer", "Failed to create acceptor event loop");
        return false;
    }
    st->loop = lp;
    hloop_set_userdata(lp, st.get());

    // threadNum 表示总 IO 线程数：1 = acceptor 兼 worker；>1 = 1 acceptor + (n-1) workers。
    int workerCount = config.threadNum > 1 ? config.threadNum - 1 : 0;
    for (int i = 0; i < workerCount; ++i) {
        hloop_t* wl = hloop_new(HLOOP_FLAG_AUTO_FREE);
        if (!wl) {
            NET_LOG_ERROR("TcpServer", "Failed to create worker event loop");
            break;
        }
        st->workerLoops.push_back(wl);
        st->workerThreads.push_back(std::make_unique<std::thread>([wl]() { hloop_run(wl); }));
    }
    st->ownThread = std::make_unique<std::thread>([lp]() { hloop_run(lp); });

    st->listenIo = hloop_create_tcp_server(lp, config.host.c_str(), config.port, &State::onAccept);
    if (!st->listenIo) {
        NET_LOG_ERROR("TcpServer", "Failed to create TCP server on " + config.host + ":" + std::to_string(config.port));
        hloop_stop(lp);
        for (void* wl : st->workerLoops) hloop_stop(static_cast<hloop_t*>(wl));
        if (st->ownThread && st->ownThread->joinable()) st->ownThread->join();
        for (auto& t : st->workerThreads) if (t && t->joinable()) t->join();
        st->ownThread.reset();
        st->workerThreads.clear();
        st->workerLoops.clear();
        st->loop = nullptr;
        return false;
    }
    st->running.store(true);
    NET_LOG_INFO("TcpServer", "TCP server started on " + config.host + ":" + std::to_string(config.port)
        + " with " + std::to_string(1 + (int)st->workerLoops.size()) + " IO loop(s)");
    return true;
}

void TcpServer::stop() {
    auto st = state_;
    std::lock_guard<std::mutex> lk(st->opMutex);
    if (!st->running.load() && !st->loop) return;
    st->running.store(false);

    void* acceptorLoop = st->loop;
    std::vector<void*> workers = st->workerLoops;
    // 停止所有 loop（hloop_stop 可从其它线程调用），随后 join 确保回调全部退出。
    if (acceptorLoop) hloop_stop(static_cast<hloop_t*>(acceptorLoop));
    for (void* wl : workers) hloop_stop(static_cast<hloop_t*>(wl));
    if (st->ownThread && st->ownThread->joinable()) st->ownThread->join();
    for (auto& t : st->workerThreads) if (t && t->joinable()) t->join();

    st->ownThread.reset();
    st->workerThreads.clear();
    st->workerLoops.clear();
    st->loop = nullptr;
    st->listenIo = nullptr;
    {
        std::lock_guard<std::mutex> cm(st->connectionsMutex);
        st->connections.clear();
    }
    NET_LOG_INFO("TcpServer", "TCP server stopped");
}

bool TcpServer::isRunning() const {
    return state_->running.load();
}

bool TcpServer::send(int connId, const std::string& data) {
    return send(connId, data.data(), data.size());
}

bool TcpServer::send(int connId, const char* data, size_t len) {
    auto st = state_;
    if (!life_->alive.load() || !st->running.load()) return false;
    void* targetLoop = nullptr;
    {
        std::lock_guard<std::mutex> lk(st->connectionsMutex);
        auto it = st->connections.find(connId);
        if (it == st->connections.end()) return false;
        targetLoop = it->second.loop;
    }
    if (!targetLoop) return false;
    std::string copy(data, len);
    postToLoop(targetLoop, [st, connId, copy]() {
        // 在连接所属 loop 内重新查表确认连接仍存活，避免对已释放/已关闭的 hio 写入。
        void* conn = nullptr;
        {
            std::lock_guard<std::mutex> lk(st->connectionsMutex);
            auto it = st->connections.find(connId);
            if (it != st->connections.end()) conn = it->second.io;
        }
        if (conn && !hio_is_closed(static_cast<hio_t*>(conn))) {
            hio_write(static_cast<hio_t*>(conn), copy.data(), (int)copy.size());
        }
    });
    return true;
}

void TcpServer::broadcast(const std::string& data) {
    auto st = state_;
    if (!life_->alive.load() || !st->running.load()) return;
    std::vector<std::pair<int, void*>> targets;
    {
        std::lock_guard<std::mutex> lk(st->connectionsMutex);
        targets.reserve(st->connections.size());
        for (auto& kv : st->connections) targets.emplace_back(kv.first, kv.second.loop);
    }
    std::string copy(data);
    for (auto& t : targets) {
        if (!t.second) continue;
        int connId = t.first;
        postToLoop(t.second, [st, connId, copy]() {
            void* conn = nullptr;
            {
                std::lock_guard<std::mutex> lk(st->connectionsMutex);
                auto it = st->connections.find(connId);
                if (it != st->connections.end()) conn = it->second.io;
            }
            if (conn && !hio_is_closed(static_cast<hio_t*>(conn))) {
                hio_write(static_cast<hio_t*>(conn), copy.data(), (int)copy.size());
            }
        });
    }
}

void TcpServer::closeConnection(int connId) {
    auto st = state_;
    TcpServerConnCtx entry;
    {
        std::lock_guard<std::mutex> lk(st->connectionsMutex);
        auto it = st->connections.find(connId);
        if (it == st->connections.end()) return;
        entry = it->second;
        st->connections.erase(it);
    }
    if (!entry.loop) return;
    // 不提前清空 context：让 close 回调统一负责断开通知。
    postToLoop(entry.loop, [entry]() {
        hio_close(static_cast<hio_t*>(entry.io));
    });
}

int TcpServer::getConnectionCount() const {
    std::lock_guard<std::mutex> lk(state_->connectionsMutex);
    return static_cast<int>(state_->connections.size());
}

std::vector<int> TcpServer::getConnectionIds() const {
    std::lock_guard<std::mutex> lk(state_->connectionsMutex);
    std::vector<int> ids;
    ids.reserve(state_->connections.size());
    for (const auto& kv : state_->connections) ids.push_back(kv.first);
    return ids;
}

void TcpServer::setMessageCallback(TcpMessageCallback callback) {
    std::lock_guard<std::mutex> lk(state_->cbMutex);
    state_->messageCallback = std::move(callback);
}

void TcpServer::setConnectionCallback(TcpConnectionCallback callback) {
    std::lock_guard<std::mutex> lk(state_->cbMutex);
    state_->connectionCallback = std::move(callback);
}

void TcpServer::setDisconnectionCallback(TcpConnectionCallback callback) {
    std::lock_guard<std::mutex> lk(state_->cbMutex);
    state_->disconnectionCallback = std::move(callback);
}

void TcpServer::setErrorCallback(TcpErrorCallback callback) {
    std::lock_guard<std::mutex> lk(state_->cbMutex);
    state_->errorCallback = std::move(callback);
}

void TcpServer::setEventLoopPool(EventLoopPool* pool) {
    // API 保留兼容；TcpServer 内部使用独占 acceptor loop（见 start() 说明）。
    std::lock_guard<std::mutex> lk(state_->opMutex);
    state_->eventLoopPool = pool;
}

void TcpServer::setFrameDecoder(const TcpFrameDecoder& decoder) {
    std::lock_guard<std::mutex> lk(state_->cbMutex);
    state_->frameDecoder = decoder;
}

void TcpServer::setLineFrameMode(char delimiter) {
    setFrameDecoder([delimiter](std::string& buf, std::string& frame) {
        size_t pos = buf.find(delimiter);
        if (pos == std::string::npos) return false;
        frame.assign(buf, 0, pos);
        buf.erase(0, pos + 1);
        return true;
    });
}

} // namespace net_sdk
