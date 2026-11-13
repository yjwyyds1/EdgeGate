#include "edgegate/proxy.hpp"

#include <fcntl.h>
#include <signal.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <deque>
#include <iomanip>
#include <map>
#include <memory>
#include <sstream>
#include <unordered_map>
#include <vector>

#include "edgegate/admin.hpp"
#include "edgegate/backend.hpp"
#include "edgegate/event_loop.hpp"
#include "edgegate/http.hpp"
#include "edgegate/log.hpp"
#include "edgegate/retry.hpp"
#include "edgegate/tls.hpp"
#include "edgegate/util.hpp"

#ifdef EDGEGATE_HAVE_NGHTTP2
#include <nghttp2/nghttp2.h>
#endif

namespace eg {
namespace {

constexpr std::size_t kRecvChunk = 64u * 1024;
constexpr std::size_t kMaxClients = 65536;
constexpr std::size_t kTraceCapacity = 256;

bool eagain(int e) { return e == EAGAIN || e == EWOULDBLOCK; }

std::string sanitize_request_id(std::string_view input) {
    if (input.empty() || input.size() > 64) return {};
    for (unsigned char c : input) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
        if (!ok) return {};
    }
    return std::string(input);
}

std::string trace_json_object(const RequestTrace& t) {
    std::ostringstream os;
    os << "{"
       << "\"id\":\"" << json_escape(t.id) << "\","
       << "\"method\":\"" << json_escape(t.method) << "\","
       << "\"path\":\"" << json_escape(t.target) << "\","
       << "\"client\":\"" << json_escape(t.client_ip) << "\","
       << "\"status\":" << t.status << ","
       << "\"backend\":\"" << json_escape(t.backend) << "\","
       << "\"attempts\":" << t.attempts << ","
       << "\"duration_ms\":" << std::fixed << std::setprecision(3) << t.duration_ms << ","
       << "\"ttfb_ms\":" << t.ttfb_ms << ","
       << "\"connect_ms\":" << t.connect_ms << ","
       << "\"bytes_in\":" << t.bytes_in << ","
       << "\"bytes_out\":" << t.bytes_out << ","
       << "\"keep_alive\":" << (t.keep_alive ? "true" : "false") << ","
       << "\"chunked_request\":" << (t.chunked_request ? "true" : "false") << ","
       << "\"chunked_response\":" << (t.chunked_response ? "true" : "false") << ","
       << "\"retried\":" << (t.retried ? "true" : "false") << ","
       << "\"started_unix_ms\":" << t.start_unix_ms << ","
       << "\"error\":\"" << json_escape(t.error) << "\""
       << "}";
    return os.str();
}

}  // namespace

struct ProxyServer::Impl : EventHandler {
    struct ClientConn;
    struct UpstreamConn;
    struct HealthConn;
    struct H2Session;
    struct H2Stream;
    struct UpstreamOwner;

    Impl(Config config, ProxyServer* owner);
    ~Impl();

    bool init(std::string& error);
    int run();
    void request_stop();
    void on_event(int fd, std::uint32_t events) override;

    void accept_clients(bool tls);
    void handle_signal();
    void tick(std::int64_t now);
    void reap();
    void shutdown_cleanup();

    Backend* select_backend(const std::vector<Backend*>& exclude);
    std::vector<BackendSnapshot> snapshots() const;

    UpstreamConn* acquire_upstream(Backend* backend, UpstreamOwner* owner, std::string& error,
                                   bool& connect_failed, bool& pool_full);
    void release_upstream(UpstreamConn* up, bool reusable);
    void destroy_upstream(UpstreamConn* up, bool abort);
    void destroy_health(HealthConn* hc);

    void start_health(Backend* backend);
    void sweep_idle(std::int64_t now);
    void update_global_gauges();
    void write_access_log(const RequestTrace& trace);
    void commit_trace(const RequestTrace& trace);

    std::string status_json() const;
    std::string recent_requests_json(std::size_t limit) const;
    std::string trace_json(std::string_view id) const;
    bool set_backend_draining(std::string_view name, bool draining, std::string& error);

    Config cfg;
    ProxyServer* outer = nullptr;
    EventLoop loop;
    std::vector<std::unique_ptr<Backend>> backend_store;
    std::vector<Backend*> backends;
    Balancer balancer;

    Metrics metrics;
    std::vector<BackendSnapshot> last_snapshots;

    int listen_fd = -1;
    int tls_listen_fd = -1;
    int signal_fd = -1;
    int access_fd = -1;
    std::unique_ptr<TlsContext> tls_ctx;
    bool shutting_down = false;
    bool global_backpressure = false;
    std::int64_t shutdown_deadline_ms = 0;
    std::int64_t start_ms = 0;

    std::unique_ptr<AdminServer> admin;

    std::unordered_map<int, std::unique_ptr<ClientConn>> clients;
    std::unordered_map<int, std::unique_ptr<UpstreamConn>> upstreams;
    std::unordered_map<int, std::unique_ptr<HealthConn>> healths;
    std::unordered_map<Backend*, std::deque<UpstreamConn*>> idle_pool;
    std::vector<int> zombie_clients;
    std::vector<int> zombie_upstreams;
    std::vector<int> zombie_healths;

    std::array<RequestTrace, kTraceCapacity> traces;
    std::size_t trace_count = 0;
    std::size_t trace_pos = 0;
};

struct ProxyServer::Impl::UpstreamOwner {
    virtual ~UpstreamOwner() = default;
    virtual void on_upstream_ready() = 0;
    virtual void on_upstream_failure(std::string reason, bool connect_failure,
                                     int status_hint) = 0;
    virtual void on_upstream_readable() = 0;
    virtual void on_upstream_eof() = 0;
    virtual void on_upstream_writable() = 0;
};

struct ProxyServer::Impl::UpstreamConn : EventHandler {
    Impl* server = nullptr;
    Backend* backend = nullptr;
    UpstreamOwner* owner = nullptr;
    int fd = -1;
    Buffer in;
    Buffer out;
    bool connecting = false;
    bool connected = false;
    bool assigned = false;
    bool idle = false;
    bool dead = false;
    bool read_paused = false;
    bool h2_mode = false;
    bool h2_ready = false;
    bool h2_handshake_done = false;
#ifdef EDGEGATE_HAVE_NGHTTP2
    nghttp2_session* h2c = nullptr;
    int32_t h2_stream_id = -1;
    // 上游 h2 响应缓冲（单 stream / 连接）
    std::string h2_status;
    std::vector<Header> h2_headers;
    std::string h2_body;
    bool h2_headers_done = false;
    bool h2_body_done = false;
    bool h2_failed = false;
    std::string h2_error;
#endif
    std::int64_t connect_start_ms = 0;
    std::int64_t idle_since_ms = 0;
    std::uint32_t events = 0;

    UpstreamConn(Impl* s, Backend* b) : server(s), backend(b) {
        if (b && b->http2()) h2_mode = true;
    }
    ~UpstreamConn() override {
#ifdef EDGEGATE_HAVE_NGHTTP2
        if (h2c) {
            nghttp2_session_del(h2c);
            h2c = nullptr;
        }
#endif
    }

    void update_events();
    void on_event(int event_fd, std::uint32_t ev) override;
    void set_read_paused(bool paused);
    void finish_connect();
    void reset_transport();
    bool start_h2_client(std::string& error);
    void stop_h2_client();
    bool h2_submit_request(const RequestHead& req, std::string_view body, std::string_view authority,
                           std::string_view client_ip, std::string_view request_id, bool preserve_host,
                           std::string& error);
    void h2_pump_read();
    void h2_pump_write();
    void h2_feed(const std::uint8_t* data, std::size_t len);
};

struct ProxyServer::Impl::ClientConn : EventHandler, UpstreamOwner {
    enum class State {
        ReadRequest,
        Dispatching,
        Connecting,
        SendingBody,
        WaitingResponse,
        StreamingResponse,
        Tunneling,
        Draining,
        Closing
    };

    Impl* server = nullptr;
    int fd = -1;
    int key_fd = -1;
    std::string client_ip;
    std::uint32_t events = 0;
    bool closed = false;

    Buffer in;
    Buffer out;
    RequestHeadParser head_parser;
    ChunkedDecoder request_chunks;
    RequestHead req;
    BodyMode req_body_mode = BodyMode::None;
    std::uint64_t req_body_remaining = 0;
    std::uint64_t req_body_seen = 0;
    bool req_head_done = false;
    bool req_body_done = false;
    bool sent_100 = false;
    std::string request_id;

    Backend* backend = nullptr;
    UpstreamConn* up = nullptr;
    std::vector<Backend*> tried;
    int attempts = 0;

    bool any_request_bytes_written = false;
    bool body_forwarded = false;
    bool replay_ok = true;
    bool replay_pending = false;
    std::string replay_body;

    ResponseHeadParser resp_parser;
    ChunkedDecoder resp_chunks;
    ResponseHead resp;
    BodyMode resp_body_mode = BodyMode::None;
    std::uint64_t resp_remaining = 0;
    bool resp_head_done = false;
    bool response_started = false;
    bool response_complete = false;
    bool client_chunked_response = false;
    bool close_upstream_after_response = false;
    bool request_body_aborted = false;

    bool keep_client_alive = true;
    bool close_after_flush = false;
    bool client_eof = false;
    bool client_paused = false;
    bool upstream_paused = false;
    bool request_active = false;
    bool trace_committed = false;
    bool websocket_upgrade = false;
    bool connect_tunnel = false;
    bool tunnel_mode = false;

    std::unique_ptr<TlsConnection> tls;
    bool tls_handshaking = false;
    bool tls_want_write = false;
    bool io_want_read = false;
    bool io_want_write = false;
    std::int64_t tls_started_ms = 0;

    std::unique_ptr<H2Session> h2;
    bool h2_mode = false;
    bool close_requested = false;

    State state = State::ReadRequest;
    std::int64_t last_activity_ms = 0;
    std::int64_t head_started_ms = 0;
    std::int64_t request_started_ms = 0;
    std::int64_t request_sent_ms = 0;
    std::int64_t last_out_progress_ms = 0;
    std::int64_t connect_start_ms = 0;
    double connect_ms = 0;
    double ttfb_ms = 0;
    int response_status = 0;
    std::uint64_t bytes_in = 0;
    std::uint64_t bytes_out = 0;

    RequestTrace trace;

    ClientConn(Impl* s, int socket_fd, std::string ip, SSL_CTX* tls_ctx = nullptr)
        : server(s),
          fd(socket_fd),
          key_fd(socket_fd),
          client_ip(std::move(ip)),
          head_parser(s->cfg.max_request_header_bytes),
          resp_parser(s->cfg.max_response_header_bytes) {
        last_activity_ms = now_ms();
        last_out_progress_ms = last_activity_ms;
        if (tls_ctx) {
            tls = std::make_unique<TlsConnection>(tls_ctx, socket_fd);
            tls_handshaking = true;
            tls_started_ms = last_activity_ms;
        }
    }
    ~ClientConn() override;

    void start();
    void start_h2();
    void set_events(std::uint32_t want);
    IoStatus client_read(void* buffer, std::size_t size, std::size_t& got);
    IoStatus client_write(const void* buffer, std::size_t size, std::size_t& wrote);
    void handle_tls_handshake(std::uint32_t ev);

    void on_event(int event_fd, std::uint32_t ev) override;
    void on_readable();
    void on_writable();
    void on_client_eof();
    void on_tick(std::int64_t now);

    bool read_allowed() const;
    void update_events();
    void handle_input();
    void dispatch();
    void try_attempt();
    void on_upstream_ready();
    void on_upstream_failure(std::string reason, bool connect_failure, int status_hint);
    void on_upstream_readable();
    void on_upstream_eof();
    void on_upstream_writable();
    void pump_request_body();
    void queue_upstream_body(std::string_view data, bool frame_chunked);
    void capture_replay(std::string_view data);
    void append_replay_body();
    void flush_upstream_out();
    void flush_client_out();
    void process_upstream_input();
    void write_client_body(std::string_view data);
    void finish_response();
    void enter_tunnel(const char* reason);
    void pump_tunnel_from_client();
    void pump_tunnel_from_upstream();
    void send_error(int status, std::string_view message);
    void reset_for_next();
    void close_now();
    void set_client_paused(bool paused, bool count);
    void set_upstream_paused(bool paused, bool count);
    void maybe_resume_upstream();
    void start_trace();
    void commit_trace(std::string_view error = {});
    bool can_retry_now() const;
};

struct ProxyServer::Impl::HealthConn : EventHandler {
    Impl* server = nullptr;
    Backend* backend = nullptr;
    int fd = -1;
    Buffer in;
    std::string request;
    bool connecting = false;
    bool reading = false;
    bool dead = false;
    bool finished = false;
    std::int64_t start_ms = 0;
    std::uint32_t events = 0;

    HealthConn(Impl* s, Backend* b) : server(s), backend(b) { start_ms = now_ms(); }
    ~HealthConn() override = default;

    void on_event(int event_fd, std::uint32_t ev) override;
    void update_events();
    void send_request();
    void finish(bool ok, std::string_view why);
    void on_tick(std::int64_t now);
};

#include "h2_impl.inc"

// ---------------------------------------------------------------------------
// Impl
// ---------------------------------------------------------------------------

ProxyServer::Impl::Impl(Config config, ProxyServer* owner)
    : cfg(std::move(config)), outer(owner), balancer(cfg.balancer) {}

ProxyServer::Impl::~Impl() {
    if (listen_fd >= 0) ::close(listen_fd);
    if (tls_listen_fd >= 0) ::close(tls_listen_fd);
    if (signal_fd >= 0) ::close(signal_fd);
    if (access_fd >= 0) ::close(access_fd);
}

bool ProxyServer::Impl::init(std::string& error) {
    start_ms = now_ms();
    if (!loop.init(error)) return false;

    for (const BackendConfig& bc : cfg.backends) {
        backend_store.push_back(std::make_unique<Backend>(bc, cfg));
        backends.push_back(backend_store.back().get());
    }
    metrics.healthy_backends.set(static_cast<std::int64_t>(backends.size()));

    listen_fd = create_listener(cfg.listen, 512, error, cfg.num_workers > 1);
    if (listen_fd < 0) return false;
    if (!loop.add(listen_fd, EPOLLIN, this)) {
        error = "cannot register listener";
        return false;
    }

    if (cfg.tls_enabled) {
        tls_ctx = std::make_unique<TlsContext>();
        TlsOptions options{cfg.tls_cert, cfg.tls_key, cfg.tls_min_version, cfg.http2};
        std::string tls_error;
        if (!tls_ctx->init(options, tls_error)) {
            error = "TLS init failed: " + tls_error;
            return false;
        }
        tls_listen_fd = create_listener(cfg.tls_listen, 512, error, cfg.num_workers > 1);
        if (tls_listen_fd < 0) {
            error = "TLS listener: " + error;
            return false;
        }
        if (!loop.add(tls_listen_fd, EPOLLIN, this)) {
            error = "cannot register TLS listener";
            return false;
        }
        log_msg(LogLevel::Info, "TLS listening on ", cfg.tls_listen.authority(), " (ALPN ",
                cfg.http2 ? "h2,http/1.1" : "http/1.1", ", min TLS ", cfg.tls_min_version, ")");
#ifndef EDGEGATE_HAVE_NGHTTP2
        if (cfg.http2) {
            log_msg(LogLevel::Warn,
                    "http2=true but EdgeGate was built without nghttp2; ALPN will only offer http/1.1");
        }
#endif
    }

    ::signal(SIGPIPE, SIG_IGN);
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGHUP);
    if (::pthread_sigmask(SIG_BLOCK, &mask, nullptr) != 0) {
        error = "pthread_sigmask failed";
        return false;
    }
    signal_fd = ::signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
    if (signal_fd < 0) {
        error = "signalfd failed";
        return false;
    }
    if (!loop.add(signal_fd, EPOLLIN, this)) {
        error = "cannot register signalfd";
        return false;
    }

    if (cfg.access_log != "-" && cfg.access_log != "off" && cfg.access_log != "none") {
        access_fd = ::open(cfg.access_log.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
        if (access_fd < 0) {
            error = "cannot open access log: " + cfg.access_log;
            return false;
        }
    }

    if (cfg.admin_enabled) {
        admin = std::make_unique<AdminServer>(*outer, cfg, loop);
        if (!admin->init(error)) return false;
    }
    return true;
}

void ProxyServer::Impl::on_event(int fd, std::uint32_t ev) {
    if (fd == listen_fd) {
        if (ev & EPOLLIN) accept_clients(false);
        return;
    }
    if (fd == tls_listen_fd) {
        if (ev & EPOLLIN) accept_clients(true);
        return;
    }
    if (fd == signal_fd) {
        if (ev & EPOLLIN) handle_signal();
    }
}

void ProxyServer::Impl::accept_clients(bool tls) {
    const int listener = tls ? tls_listen_fd : listen_fd;
    for (;;) {
        sockaddr_storage ss{};
        socklen_t len = sizeof(ss);
        const int fd = ::accept4(listener, reinterpret_cast<sockaddr*>(&ss), &len,
                                 SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd < 0) {
            if (errno == EINTR) continue;
            if (!eagain(errno)) log_msg(LogLevel::Warn, "accept failed: ", std::strerror(errno));
            break;
        }
        if (shutting_down || clients.size() >= kMaxClients) {
            ::close(fd);
            continue;
        }
        set_tcp_nodelay(fd);
        set_keepalive(fd);
        const std::string ip = peer_ip(fd);
        SSL_CTX* ssl_ctx = tls && tls_ctx ? tls_ctx->ctx() : nullptr;
        auto client = std::make_unique<ClientConn>(this, fd, ip, ssl_ctx);
        ClientConn* raw = client.get();
        clients.emplace(fd, std::move(client));
        if (!loop.add(fd, EPOLLIN | EPOLLRDHUP, raw)) {
            clients.erase(fd);
            ::close(fd);
            continue;
        }
        raw->start();
        metrics.client_connections_total.inc();
        metrics.active_clients.inc();
    }
}

void ProxyServer::Impl::handle_signal() {
    signalfd_siginfo info{};
    for (;;) {
        const ssize_t n = ::read(signal_fd, &info, sizeof(info));
        if (n != static_cast<ssize_t>(sizeof(info))) break;
        if (info.ssi_signo == SIGINT || info.ssi_signo == SIGTERM) {
            log_msg(LogLevel::Info, "received signal ", info.ssi_signo, ", shutting down");
            request_stop();
        } else if (info.ssi_signo == SIGHUP) {
            log_msg(LogLevel::Warn, "SIGHUP received; config reload is not implemented in v1");
        }
    }
}

void ProxyServer::Impl::request_stop() {
    if (shutting_down) return;
    shutting_down = true;
    shutdown_deadline_ms = now_ms() + cfg.shutdown_grace_ms;
    if (listen_fd >= 0) {
        loop.close_later(listen_fd);
        listen_fd = -1;
    }
    if (tls_listen_fd >= 0) {
        loop.close_later(tls_listen_fd);
        tls_listen_fd = -1;
    }
    if (admin) admin->close();
    log_msg(LogLevel::Info, "stopping: no new connections, grace ", cfg.shutdown_grace_ms, " ms");
}

Backend* ProxyServer::Impl::select_backend(const std::vector<Backend*>& exclude) {
    return balancer.select(backends, exclude, now_ms());
}

std::vector<BackendSnapshot> ProxyServer::Impl::snapshots() const {
    std::vector<BackendSnapshot> out;
    out.reserve(backends.size());
    for (Backend* b : backends) out.push_back(b->snapshot());
    return out;
}

ProxyServer::Impl::UpstreamConn* ProxyServer::Impl::acquire_upstream(Backend* backend,
                                                                     UpstreamOwner* owner,
                                                                     std::string& error,
                                                                     bool& connect_failed,
                                                                     bool& pool_full) {
    connect_failed = false;
    pool_full = false;

    auto& pool = idle_pool[backend];
    while (!pool.empty()) {
        UpstreamConn* up = pool.front();
        pool.pop_front();
        if (up->idle) {
            up->idle = false;
            backend->metrics.idle_connections.dec();
        }
        char probe = 0;
        const ssize_t n = ::recv(up->fd, &probe, 1, MSG_PEEK | MSG_DONTWAIT);
        const bool stale = n == 0 || n > 0 || (n < 0 && !eagain(errno));
        if (stale) {
            destroy_upstream(up, true);
            continue;
        }
        up->owner = owner;
        up->assigned = true;
        up->connecting = false;
        up->connected = true;
        up->read_paused = false;
        up->reset_transport();
        backend->metrics.active_connections.inc();
        loop.add(up->fd, EPOLLIN | EPOLLRDHUP, up);
        up->update_events();
        return up;
    }

    const auto total = static_cast<std::size_t>(backend->metrics.active_connections.value() +
                                                backend->metrics.idle_connections.value());
    if (total >= backend->max_connections()) {
        pool_full = true;
        error = "backend connection limit reached";
        return nullptr;
    }

    auto conn = std::make_unique<UpstreamConn>(this, backend);
    bool in_progress = false;
    std::string why;
    const int fd = connect_nonblocking(backend->endpoint(), in_progress, why);
    if (fd < 0) {
        connect_failed = true;
        backend->record_failure(now_ms(), true);
        error = why;
        return nullptr;
    }
    conn->fd = fd;
    conn->connecting = in_progress;
    conn->connected = !in_progress;
    conn->assigned = true;
    conn->owner = owner;
    conn->connect_start_ms = now_ms();
    backend->metrics.active_connections.inc();
    UpstreamConn* raw = conn.get();
    upstreams.emplace(fd, std::move(conn));
    const std::uint32_t interest = in_progress ? (EPOLLOUT | EPOLLRDHUP) : (EPOLLIN | EPOLLRDHUP);
    if (!loop.add(fd, interest, raw)) {
        error = "cannot register upstream socket";
        destroy_upstream(raw, true);
        return nullptr;
    }
    raw->events = interest;
    log_msg(LogLevel::Debug, "upstream fd ", fd, " -> ", backend->endpoint().authority(),
            " connecting=", in_progress);
    return raw;
}

void ProxyServer::Impl::release_upstream(UpstreamConn* up, bool reusable) {
    if (!up || up->dead) return;
    up->owner = nullptr;
    if (up->assigned) {
        up->assigned = false;
        up->backend->metrics.active_connections.dec();
    }
    const bool can_pool = reusable && !shutting_down && !up->dead && up->backend &&
                          !up->h2_mode &&
                          static_cast<std::size_t>(up->backend->metrics.idle_connections.value()) <
                              up->backend->max_idle();
    if (can_pool) {
        up->idle = true;
        up->idle_since_ms = now_ms();
        up->connecting = false;
        up->connected = false;
        up->read_paused = false;
        up->in.clear();
        up->out.clear();
        loop.remove(up->fd);
        up->backend->metrics.idle_connections.inc();
        idle_pool[up->backend].push_back(up);
        return;
    }
    destroy_upstream(up, false);
}

void ProxyServer::Impl::destroy_upstream(UpstreamConn* up, bool abort) {
    (void)abort;
    if (!up || up->dead) return;
    up->dead = true;
    up->owner = nullptr;
    up->stop_h2_client();
    if (up->idle) {
        up->idle = false;
        up->backend->metrics.idle_connections.dec();
        auto& pool = idle_pool[up->backend];
        pool.erase(std::remove(pool.begin(), pool.end(), up), pool.end());
    }
    if (up->assigned) {
        up->assigned = false;
        up->backend->metrics.active_connections.dec();
    }
    if (up->fd >= 0) {
        loop.close_later(up->fd);
        zombie_upstreams.push_back(up->fd);
    }
}

void ProxyServer::Impl::destroy_health(HealthConn* hc) {
    if (!hc || hc->dead) return;
    hc->dead = true;
    if (hc->backend) {
        hc->backend->set_health_check_in_progress(false);
        hc->backend->mark_health_check(now_ms());
    }
    if (hc->fd >= 0) {
        loop.close_later(hc->fd);
        zombie_healths.push_back(hc->fd);
    }
}

void ProxyServer::Impl::start_health(Backend* backend) {
    if (cfg.health_interval_ms <= 0) return;
    backend->set_health_check_in_progress(true);
    auto hc = std::make_unique<HealthConn>(this, backend);
    bool in_progress = false;
    std::string why;
    const int fd = connect_nonblocking(backend->endpoint(), in_progress, why);
    if (fd < 0) {
        metrics.health_checks.inc();
        metrics.health_failures.inc();
        backend->record_failure(now_ms(), false);
        backend->set_health_check_in_progress(false);
        backend->mark_health_check(now_ms());
        return;
    }
    hc->fd = fd;
    hc->connecting = in_progress;
    hc->reading = !in_progress;
    HealthConn* raw = hc.get();
    healths.emplace(fd, std::move(hc));
    const std::uint32_t interest = in_progress ? (EPOLLOUT | EPOLLRDHUP) : (EPOLLIN | EPOLLRDHUP);
    loop.add(fd, interest, raw);
    raw->events = interest;
    if (!in_progress) raw->send_request();
}

void ProxyServer::Impl::sweep_idle(std::int64_t now) {
    for (auto& [backend, pool] : idle_pool) {
        while (!pool.empty()) {
            UpstreamConn* up = pool.front();
            if (now - up->idle_since_ms < cfg.backend_idle_timeout_ms) break;
            pool.pop_front();
            up->idle = false;
            backend->metrics.idle_connections.dec();
            destroy_upstream(up, false);
        }
    }
}

void ProxyServer::Impl::update_global_gauges() {
    std::int64_t buffered = 0;
    std::int64_t active_requests = 0;
    for (auto& [fd, c] : clients) {
        if (c->closed) continue;
        buffered += static_cast<std::int64_t>(c->in.size() + c->out.size());
        if (c->request_active) ++active_requests;
        if (c->h2_mode && c->h2) {
            buffered += static_cast<std::int64_t>(c->h2->buffered_bytes());
            active_requests += static_cast<std::int64_t>(c->h2->active_streams());
        }
    }
    for (auto& [fd, u] : upstreams) {
        if (u->dead) continue;
        buffered += static_cast<std::int64_t>(u->in.size() + u->out.size());
    }
    metrics.buffered_bytes.set(buffered);
    metrics.active_requests.set(active_requests);

    const auto total = static_cast<std::size_t>(std::max<std::int64_t>(0, buffered));
    if (!global_backpressure && total > cfg.max_total_buffered) {
        global_backpressure = true;
        log_msg(LogLevel::Warn, "global buffer budget exceeded: ", total, " bytes; pausing reads");
    } else if (global_backpressure && total < cfg.max_total_buffered * 3 / 4) {
        global_backpressure = false;
        log_msg(LogLevel::Info, "global buffer budget recovered: ", total, " bytes");
    }

    int healthy = 0;
    for (Backend* b : backends) {
        if (b->healthy()) ++healthy;
    }
    metrics.healthy_backends.set(healthy);
}

void ProxyServer::Impl::tick(std::int64_t now) {
    update_global_gauges();

    for (auto& [fd, c] : clients) {
        if (!c->closed) c->on_tick(now);
    }
    for (auto& [fd, u] : upstreams) {
        if (u->dead || !u->connecting || !u->owner) continue;
        if (now - u->connect_start_ms > cfg.upstream_connect_timeout_ms) {
            u->owner->on_upstream_failure("upstream connect timeout", true, 504);
        }
    }
    for (auto& [fd, h] : healths) {
        if (!h->dead) h->on_tick(now);
    }

    if (!global_backpressure) {
        for (auto& [fd, c] : clients) {
            if (!c->closed) c->maybe_resume_upstream();
        }
    }

    for (Backend* b : backends) {
        if (b->health_check_due(now, cfg.health_interval_ms)) start_health(b);
    }
    sweep_idle(now);
    if (admin) admin->on_tick(now);
}

void ProxyServer::Impl::reap() {
    for (int fd : zombie_clients) clients.erase(fd);
    for (int fd : zombie_upstreams) upstreams.erase(fd);
    for (int fd : zombie_healths) healths.erase(fd);
    zombie_clients.clear();
    zombie_upstreams.clear();
    zombie_healths.clear();
}

int ProxyServer::Impl::run() {
    log_msg(LogLevel::Info, "edgegate worker ", cfg.worker_id, "/", cfg.num_workers,
            " pid ", static_cast<long>(::getpid()), " listening on ", cfg.listen.authority(),
            " admin ", cfg.admin_enabled ? cfg.admin.authority() : "off");
    while (!loop.stopped()) {
        loop.run_once(100);
        reap();
        const std::int64_t now = now_ms();
        tick(now);

        if (shutting_down) {
            const bool no_clients = clients.empty();
            if (no_clients || now >= shutdown_deadline_ms) {
                if (!no_clients) {
                    log_msg(LogLevel::Warn, "shutdown grace expired with ", clients.size(),
                            " client(s) still connected");
                }
                std::vector<ClientConn*> remaining;
                remaining.reserve(clients.size());
                for (auto& [fd, c] : clients) remaining.push_back(c.get());
                for (ClientConn* c : remaining) c->close_now();
                std::vector<UpstreamConn*> ups;
                ups.reserve(upstreams.size());
                for (auto& [fd, u] : upstreams) ups.push_back(u.get());
                for (UpstreamConn* u : ups) destroy_upstream(u, true);
                loop.flush_closes();
                reap();
                loop.stop();
            }
        }
    }
    shutdown_cleanup();
    return 0;
}

void ProxyServer::Impl::shutdown_cleanup() {
    if (admin) admin->close();
    for (auto& [fd, c] : clients) c->close_now();
    std::vector<UpstreamConn*> ups;
    ups.reserve(upstreams.size());
    for (auto& [fd, u] : upstreams) ups.push_back(u.get());
    for (UpstreamConn* u : ups) destroy_upstream(u, true);
    std::vector<HealthConn*> hs;
    hs.reserve(healths.size());
    for (auto& [fd, h] : healths) hs.push_back(h.get());
    for (HealthConn* h : hs) destroy_health(h);
    loop.flush_closes();
    if (listen_fd >= 0) {
        ::close(listen_fd);
        listen_fd = -1;
    }
    if (tls_listen_fd >= 0) {
        ::close(tls_listen_fd);
        tls_listen_fd = -1;
    }
    if (signal_fd >= 0) {
        ::close(signal_fd);
        signal_fd = -1;
    }
    if (access_fd >= 0) {
        ::close(access_fd);
        access_fd = -1;
    }
    reap();
}

void ProxyServer::Impl::write_access_log(const RequestTrace& t) {
    if (cfg.access_log == "off" || cfg.access_log == "none") return;
    std::ostringstream os;
    os << "{\"ts\":\"" << iso8601_now() << "\",\"request_id\":\"" << json_escape(t.id)
       << "\",\"client\":\"" << json_escape(t.client_ip) << "\",\"method\":\""
       << json_escape(t.method) << "\",\"path\":\"" << json_escape(t.target)
       << "\",\"status\":" << t.status << ",\"upstream\":\"" << json_escape(t.backend)
       << "\",\"attempts\":" << t.attempts << ",\"duration_ms\":" << std::fixed
       << std::setprecision(3) << t.duration_ms << ",\"ttfb_ms\":" << t.ttfb_ms
       << ",\"bytes_in\":" << t.bytes_in << ",\"bytes_out\":" << t.bytes_out
       << ",\"retried\":" << (t.retried ? "true" : "false") << ",\"error\":\""
       << json_escape(t.error) << "\"}\n";
    const std::string line = os.str();
    if (access_fd >= 0) {
        std::size_t off = 0;
        while (off < line.size()) {
            const ssize_t n = ::write(access_fd, line.data() + off, line.size() - off);
            if (n <= 0) {
                if (errno == EINTR) continue;
                break;
            }
            off += static_cast<std::size_t>(n);
        }
    } else {
        std::fwrite(line.data(), 1, line.size(), stderr);
        std::fflush(stderr);
    }
}

void ProxyServer::Impl::commit_trace(const RequestTrace& trace) {
    traces[trace_pos] = trace;
    trace_pos = (trace_pos + 1) % kTraceCapacity;
    if (trace_count < kTraceCapacity) ++trace_count;
    write_access_log(trace);
}

std::string ProxyServer::Impl::trace_json(std::string_view id) const {
    for (std::size_t i = 0; i < trace_count; ++i) {
        const std::size_t idx = (trace_pos + kTraceCapacity - 1 - i) % kTraceCapacity;
        if (traces[idx].id == id) {
            return "{\"found\":true,\"trace\":" + trace_json_object(traces[idx]) + "}";
        }
    }
    return "{\"found\":false}";
}

std::string ProxyServer::Impl::recent_requests_json(std::size_t limit) const {
    std::ostringstream os;
    os << "[";
    const std::size_t n = std::min(limit, trace_count);
    for (std::size_t i = 0; i < n; ++i) {
        const std::size_t idx = (trace_pos + kTraceCapacity - 1 - i) % kTraceCapacity;
        if (i) os << ",";
        os << trace_json_object(traces[idx]);
    }
    os << "]";
    return os.str();
}

std::string ProxyServer::Impl::status_json() const {
    const std::int64_t uptime = (now_ms() - start_ms) / 1000;
    std::ostringstream os;
    os << "{"
       << "\"service\":\"edgegate\",\"version\":\"1.3.0\","
       << "\"uptime_seconds\":" << uptime << ","
       << "\"worker_id\":" << cfg.worker_id << ","
       << "\"num_workers\":" << cfg.num_workers << ","
       << "\"pid\":" << static_cast<long>(::getpid()) << ","
       << "\"config\":{"
       << "\"listen\":\"" << json_escape(cfg.listen.authority()) << "\","
       << "\"admin\":\"" << (cfg.admin_enabled ? json_escape(cfg.admin.authority()) : "off")
       << "\",\"balancer\":\"" << json_escape(cfg.balancer) << "\","
       << "\"num_workers\":" << cfg.num_workers << ","
       << "\"max_attempts\":" << cfg.max_attempts << ","
       << "\"preserve_host\":" << (cfg.preserve_host ? "true" : "false") << ","
       << "\"client_buffer_high\":" << cfg.client_buffer_high << ","
       << "\"client_buffer_low\":" << cfg.client_buffer_low << ","
       << "\"upstream_buffer_high\":" << cfg.upstream_buffer_high << ","
       << "\"upstream_buffer_low\":" << cfg.upstream_buffer_low << ","
       << "\"upstream_response_timeout_ms\":" << cfg.upstream_response_timeout_ms
       << "},\"metrics\":{"
       << "\"requests_total\":" << metrics.requests_total.value() << ","
       << "\"responses_total\":" << metrics.responses_total.value() << ","
       << "\"client_connections_total\":" << metrics.client_connections_total.value() << ","
       << "\"client_disconnects_total\":" << metrics.client_disconnects.value() << ","
       << "\"upstream_connections_total\":" << metrics.upstream_connections_total.value() << ","
       << "\"requests_rejected_total\":" << metrics.requests_rejected.value() << ","
       << "\"active_clients\":" << metrics.active_clients.value() << ","
       << "\"active_requests\":" << metrics.active_requests.value() << ","
       << "\"buffered_bytes\":" << metrics.buffered_bytes.value() << ","
       << "\"healthy_backends\":" << metrics.healthy_backends.value() << ","
       << "\"retries_total\":" << metrics.retries.value() << ","
       << "\"bytes_in_total\":" << metrics.bytes_in_total.value() << ","
       << "\"bytes_out_total\":" << metrics.bytes_out_total.value() << ","
       << "\"chunked_requests_total\":" << metrics.chunked_requests.value() << ","
       << "\"chunked_responses_total\":" << metrics.chunked_responses.value() << ","
       << "\"http10_requests_total\":" << metrics.http10_requests.value() << ","
       << "\"upstream_pauses\":" << metrics.upstream_read_pauses.value() << ","
       << "\"upstream_resumes\":" << metrics.upstream_read_resumes.value() << ","
       << "\"client_pauses\":" << metrics.client_read_pauses.value() << ","
       << "\"client_resumes\":" << metrics.client_read_resumes.value() << ","
       << "\"errors\":{"
       << "\"parse\":" << metrics.parse_errors.value() << ","
       << "\"connect\":" << metrics.connect_errors.value() << ","
       << "\"read\":" << metrics.read_errors.value() << ","
       << "\"write\":" << metrics.write_errors.value() << ","
       << "\"timeout\":" << metrics.timeouts.value() << ","
       << "\"no_backend\":" << metrics.no_backend.value() << ","
       << "\"body_too_large\":" << metrics.body_too_large.value() << ","
       << "\"upstream_eof\":" << metrics.upstream_eof.value() << "},"
       << "\"latency_ms\":{"
       << "\"p50\":" << metrics.request_duration_ms.quantile(0.50) << ","
       << "\"p95\":" << metrics.request_duration_ms.quantile(0.95) << ","
       << "\"p99\":" << metrics.request_duration_ms.quantile(0.99) << ","
       << "\"count\":" << metrics.request_duration_ms.count() << "}"
       << "},\"backends\":[";

    const std::vector<BackendSnapshot> snaps = snapshots();
    for (std::size_t i = 0; i < snaps.size(); ++i) {
        const BackendSnapshot& b = snaps[i];
        if (i) os << ",";
        os << "{\"name\":\"" << json_escape(b.name) << "\","
           << "\"url\":\"" << json_escape(b.url) << "\","
           << "\"healthy\":" << (b.healthy ? "true" : "false") << ","
           << "\"draining\":" << (b.draining ? "true" : "false") << ","
           << "\"active_connections\":" << b.active_connections << ","
           << "\"idle_connections\":" << b.idle_connections << ","
           << "\"requests\":" << b.requests << ","
           << "\"errors\":" << b.errors << ","
           << "\"retries\":" << b.retries << ","
           << "\"connect_failures\":" << b.connect_failures << ","
           << "\"responses_2xx\":" << b.responses_2xx << ","
           << "\"responses_3xx\":" << b.responses_3xx << ","
           << "\"responses_4xx\":" << b.responses_4xx << ","
           << "\"responses_5xx\":" << b.responses_5xx << ","
           << "\"latency_p50\":" << b.latency_p50 << ","
           << "\"latency_p95\":" << b.latency_p95 << ","
           << "\"latency_p99\":" << b.latency_p99 << "}";
    }
    os << "],\"recent_requests\":" << recent_requests_json(20) << "}";
    return os.str();
}

bool ProxyServer::Impl::set_backend_draining(std::string_view name, bool draining,
                                             std::string& error) {
    for (Backend* b : backends) {
        if (b->name() == name) {
            b->set_draining(draining);
            log_msg(LogLevel::Info, "backend ", b->name(), draining ? " draining" : " enabled");
            return true;
        }
    }
    error = "unknown backend: " + std::string(name);
    return false;
}

// ---------------------------------------------------------------------------
// UpstreamConn
// ---------------------------------------------------------------------------

void ProxyServer::Impl::UpstreamConn::update_events() {
    if (fd < 0 || dead) return;
    std::uint32_t want = EPOLLRDHUP;
    if (connecting) {
        want |= EPOLLOUT;
    } else if (connected) {
        if (!read_paused) want |= EPOLLIN;
        if (!out.empty()) want |= EPOLLOUT;
#ifdef EDGEGATE_HAVE_NGHTTP2
        if (h2c && nghttp2_session_want_write(h2c)) want |= EPOLLOUT;
#endif
    }
    if (want != events) {
        events = want;
        server->loop.mod(fd, want);
    }
}

void ProxyServer::Impl::UpstreamConn::set_read_paused(bool paused) {
    if (read_paused == paused) return;
    read_paused = paused;
    update_events();
}

void ProxyServer::Impl::UpstreamConn::finish_connect() {
    int err = 0;
    socklen_t len = sizeof(err);
    if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) != 0 || err != 0) {
        const std::string why = err ? std::strerror(err) : "connect failed";
        log_msg(LogLevel::Debug, "upstream fd ", fd, " connect error: ", why);
        if (owner) owner->on_upstream_failure("upstream connect: " + why, true, 502);
        return;
    }
    connecting = false;
    connected = true;
    if (h2_mode) {
#ifdef EDGEGATE_HAVE_NGHTTP2
        std::string why;
        if (!start_h2_client(why)) {
            if (owner) owner->on_upstream_failure(why.empty() ? "h2 client init failed" : why, true, 502);
            return;
        }
#else
        if (owner) {
            owner->on_upstream_failure("backend http2=true but EdgeGate built without nghttp2", true, 502);
        }
        return;
#endif
    }
    update_events();
    log_msg(LogLevel::Debug, "upstream fd ", fd, " connected", h2_mode ? " (h2)" : "");
    if (owner) owner->on_upstream_ready();
}

void ProxyServer::Impl::UpstreamConn::on_event(int event_fd, std::uint32_t ev) {
    (void)event_fd;
    if (dead) return;
    if (connecting) {
        if (ev & (EPOLLERR | EPOLLHUP)) {
            if (owner) owner->on_upstream_failure("upstream connect failed", true, 502);
            return;
        }
        if (ev & EPOLLOUT) finish_connect();
        return;
    }
    if (ev & EPOLLERR) {
        if (owner) owner->on_upstream_failure("upstream socket error", false, 502);
        return;
    }
    if (ev & EPOLLOUT) {
#ifdef EDGEGATE_HAVE_NGHTTP2
        if (h2_mode && h2c) h2_pump_write();
#endif
        if (owner) owner->on_upstream_writable();
        if (dead || !owner) return;
    }
    if (ev & EPOLLIN) {
#ifdef EDGEGATE_HAVE_NGHTTP2
        if (h2_mode && h2c) {
            h2_pump_read();
            if (dead) return;
        }
#endif
        if (owner) owner->on_upstream_readable();
        if (dead || !owner) return;
    }
    if (ev & (EPOLLHUP | EPOLLRDHUP)) {
        if (owner) owner->on_upstream_eof();
    }
}

void ProxyServer::Impl::UpstreamConn::reset_transport() {
    in.clear();
    out.clear();
    events = 0;
    stop_h2_client();
    if (backend && backend->http2()) h2_mode = true;
    else h2_mode = false;
    h2_ready = false;
    h2_handshake_done = false;
}

void ProxyServer::Impl::UpstreamConn::stop_h2_client() {
#ifdef EDGEGATE_HAVE_NGHTTP2
    if (h2c) {
        nghttp2_session_del(h2c);
        h2c = nullptr;
    }
    h2_stream_id = -1;
    h2_status.clear();
    h2_headers.clear();
    h2_body.clear();
    h2_headers_done = false;
    h2_body_done = false;
    h2_failed = false;
    h2_error.clear();
#endif
    h2_ready = false;
}

bool ProxyServer::Impl::UpstreamConn::start_h2_client(std::string& error) {
#ifndef EDGEGATE_HAVE_NGHTTP2
    error = "nghttp2 not available";
    return false;
#else
    stop_h2_client();
    h2_mode = true;
    nghttp2_session_callbacks* callbacks = nullptr;
    if (nghttp2_session_callbacks_new(&callbacks) != 0 || !callbacks) {
        error = "nghttp2_session_callbacks_new failed";
        return false;
    }

    nghttp2_session_callbacks_set_on_header_callback(
        callbacks,
        [](nghttp2_session*, const nghttp2_frame* frame, const uint8_t* name, size_t namelen,
           const uint8_t* value, size_t valuelen, uint8_t, void* user_data) -> int {
            auto* self = static_cast<UpstreamConn*>(user_data);
            if (!self || frame->hd.type != NGHTTP2_HEADERS) return 0;
            if (frame->headers.cat != NGHTTP2_HCAT_RESPONSE &&
                frame->headers.cat != NGHTTP2_HCAT_HEADERS) {
                return 0;
            }
            std::string n(reinterpret_cast<const char*>(name), namelen);
            std::string v(reinterpret_cast<const char*>(value), valuelen);
            if (n == ":status") {
                self->h2_status = std::move(v);
            } else if (!n.empty() && n[0] != ':') {
                Header h;
                h.name = to_lower(n);
                h.value = std::move(v);
                self->h2_headers.push_back(std::move(h));
            }
            return 0;
        });

    nghttp2_session_callbacks_set_on_frame_recv_callback(
        callbacks,
        [](nghttp2_session*, const nghttp2_frame* frame, void* user_data) -> int {
            auto* self = static_cast<UpstreamConn*>(user_data);
            if (!self) return 0;
            if (frame->hd.type == NGHTTP2_HEADERS &&
                (frame->headers.cat == NGHTTP2_HCAT_RESPONSE ||
                 frame->headers.cat == NGHTTP2_HCAT_HEADERS) &&
                (frame->hd.flags & NGHTTP2_FLAG_END_HEADERS)) {
                if (!self->h2_headers_done) {
                    self->h2_headers_done = true;
                    // 把 h2 响应头转成 HTTP/1.1，复用现有解析路径
                    std::string synthetic;
                    synthetic.reserve(256);
                    synthetic += "HTTP/1.1 ";
                    synthetic += self->h2_status.empty() ? "200" : self->h2_status;
                    synthetic += " OK\r\n";
                    bool has_cl = false;
                    for (const Header& h : self->h2_headers) {
                        if (h.name == "connection" || h.name == "transfer-encoding") continue;
                        if (h.name == "content-length") has_cl = true;
                        synthetic += h.name;
                        synthetic += ": ";
                        synthetic += h.value;
                        synthetic += "\r\n";
                    }
                    if (!has_cl && !(frame->hd.flags & NGHTTP2_FLAG_END_STREAM)) {
                        // 流式 body：用 close-delimited（响应结束时 upstream eof）
                    }
                    synthetic += "Connection: close\r\n\r\n";
                    self->in.append(synthetic);
                }
                if (frame->hd.flags & NGHTTP2_FLAG_END_STREAM) {
                    self->h2_body_done = true;
                }
            }
            if (frame->hd.type == NGHTTP2_DATA && (frame->hd.flags & NGHTTP2_FLAG_END_STREAM)) {
                self->h2_body_done = true;
            }
            return 0;
        });

    nghttp2_session_callbacks_set_on_data_chunk_recv_callback(
        callbacks,
        [](nghttp2_session*, uint8_t, int32_t, const uint8_t* data, size_t len,
           void* user_data) -> int {
            auto* self = static_cast<UpstreamConn*>(user_data);
            if (!self || len == 0) return 0;
            self->in.append(reinterpret_cast<const char*>(data), len);
            return 0;
        });

    nghttp2_session_callbacks_set_on_stream_close_callback(
        callbacks,
        [](nghttp2_session*, int32_t, uint32_t error_code, void* user_data) -> int {
            auto* self = static_cast<UpstreamConn*>(user_data);
            if (!self) return 0;
            self->h2_body_done = true;
            if (error_code != NGHTTP2_NO_ERROR) {
                self->h2_failed = true;
                self->h2_error = "upstream h2 stream error " + std::to_string(error_code);
            }
            return 0;
        });

    const int rc = nghttp2_session_client_new2(&h2c, callbacks, this, nullptr);
    nghttp2_session_callbacks_del(callbacks);
    if (rc != 0 || !h2c) {
        error = "nghttp2_session_client_new2 failed";
        return false;
    }

    nghttp2_settings_entry settings[] = {
        {NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, 1},
        {NGHTTP2_SETTINGS_INITIAL_WINDOW_SIZE, 1u << 20},
    };
    if (nghttp2_submit_settings(h2c, NGHTTP2_FLAG_NONE, settings, 2) != 0) {
        error = "nghttp2_submit_settings failed";
        stop_h2_client();
        return false;
    }

    // prior knowledge：先写 client connection preface
    static const char kPreface[] = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
    out.append(kPreface, sizeof(kPreface) - 1);
    h2_pump_write();
    h2_ready = true;
    h2_handshake_done = true;
    return true;
#endif
}

bool ProxyServer::Impl::UpstreamConn::h2_submit_request(const RequestHead& req,
                                                         std::string_view body,
                                                         std::string_view authority,
                                                         std::string_view client_ip,
                                                         std::string_view request_id,
                                                         bool preserve_host, std::string& error) {
#ifndef EDGEGATE_HAVE_NGHTTP2
    (void)req;
    (void)body;
    (void)authority;
    (void)client_ip;
    (void)request_id;
    (void)preserve_host;
    error = "nghttp2 not available";
    return false;
#else
    if (!h2c) {
        error = "h2 client session missing";
        return false;
    }
    h2_status.clear();
    h2_headers.clear();
    h2_body.clear();
    h2_headers_done = false;
    h2_body_done = false;
    h2_failed = false;
    h2_error.clear();
    in.clear();

    std::vector<std::string> store;
    store.reserve(16);
    auto add = [&](std::string n, std::string v) {
        store.push_back(std::move(n));
        store.push_back(std::move(v));
    };
    add(":method", req.method);
    add(":path", req.target.empty() ? "/" : req.target);
    add(":scheme", "http");
    add(":authority", preserve_host && !req.host.empty() ? req.host : std::string(authority));

    for (const Header& h : req.headers) {
        if (h.name == "host" || h.name == "connection" || h.name == "keep-alive" ||
            h.name == "proxy-connection" || h.name == "transfer-encoding" || h.name == "upgrade" ||
            h.name == "te" || h.name == "trailer" || h.name == "content-length") {
            continue;
        }
        add(h.name, h.value);
    }
    if (!body.empty() || req.has_content_length) {
        add("content-length", std::to_string(body.size()));
    }
    add("x-forwarded-for", std::string(client_ip));
    add("x-forwarded-proto", "http");
    if (!request_id.empty()) add("x-request-id", std::string(request_id));
    add("via", "1.1 edgegate");

    std::vector<nghttp2_nv> nva;
    nva.reserve(store.size() / 2);
    for (std::size_t i = 0; i + 1 < store.size(); i += 2) {
        nghttp2_nv nv{};
        nv.name = reinterpret_cast<uint8_t*>(store[i].data());
        nv.value = reinterpret_cast<uint8_t*>(store[i + 1].data());
        nv.namelen = store[i].size();
        nv.valuelen = store[i + 1].size();
        nv.flags = NGHTTP2_NV_FLAG_NONE;
        nva.push_back(nv);
    }

    nghttp2_data_provider2 provider{};
    std::string* body_holder = nullptr;
    if (!body.empty()) {
        body_holder = new std::string(body);
        provider.source.ptr = body_holder;
        provider.read_callback = [](nghttp2_session*, int32_t, uint8_t* buf, size_t length,
                                    uint32_t* data_flags, nghttp2_data_source* source,
                                    void*) -> nghttp2_ssize {
            auto* s = static_cast<std::string*>(source->ptr);
            if (!s || s->empty()) {
                *data_flags |= NGHTTP2_DATA_FLAG_EOF;
                return 0;
            }
            const size_t n = std::min(length, s->size());
            std::memcpy(buf, s->data(), n);
            s->erase(0, n);
            if (s->empty()) *data_flags |= NGHTTP2_DATA_FLAG_EOF;
            return static_cast<nghttp2_ssize>(n);
        };
    }

    const int32_t sid =
        nghttp2_submit_request2(h2c, nullptr, nva.data(), nva.size(),
                                body.empty() ? nullptr : &provider, this);
    if (body_holder && sid < 0) delete body_holder;
    if (sid < 0) {
        error = "nghttp2_submit_request2 failed";
        if (body_holder) delete body_holder;
        return false;
    }
    h2_stream_id = sid;
    // 把 body_holder 挂到 stream user data 以便结束后释放
    if (body_holder) {
        nghttp2_session_set_stream_user_data(h2c, sid, body_holder);
    }
    h2_pump_write();
    update_events();
    return true;
#endif
}

void ProxyServer::Impl::UpstreamConn::h2_pump_write() {
#ifdef EDGEGATE_HAVE_NGHTTP2
    if (!h2c || dead) return;
    for (;;) {
        const uint8_t* data = nullptr;
        const ssize_t n = nghttp2_session_mem_send(h2c, &data);
        if (n < 0) {
            h2_failed = true;
            h2_error = "nghttp2_session_mem_send failed";
            return;
        }
        if (n == 0) break;
        out.append(reinterpret_cast<const char*>(data), static_cast<std::size_t>(n));
    }
    while (!out.empty()) {
        const ssize_t n = ::send(fd, out.data(), out.size(), MSG_NOSIGNAL);
        if (n > 0) {
            out.consume(static_cast<std::size_t>(n));
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else if (n < 0 && eagain(errno)) {
            break;
        } else {
            h2_failed = true;
            h2_error = std::string("upstream h2 write: ") + std::strerror(errno);
            return;
        }
    }
    update_events();
#endif
}

void ProxyServer::Impl::UpstreamConn::h2_pump_read() {
#ifdef EDGEGATE_HAVE_NGHTTP2
    if (!h2c || dead || read_paused) return;
    char buf[kRecvChunk];
    for (;;) {
        const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n > 0) {
            h2_feed(reinterpret_cast<const uint8_t*>(buf), static_cast<std::size_t>(n));
            if (h2_failed) {
                if (owner) owner->on_upstream_failure(h2_error, false, 502);
                return;
            }
            continue;
        }
        if (n == 0) {
            if (owner) owner->on_upstream_eof();
            return;
        }
        if (errno == EINTR) continue;
        if (eagain(errno)) break;
        if (owner) {
            owner->on_upstream_failure(std::string("upstream h2 read: ") + std::strerror(errno), false,
                                       502);
        }
        return;
    }
    h2_pump_write();
    update_events();
#endif
}

void ProxyServer::Impl::UpstreamConn::h2_feed(const std::uint8_t* data, std::size_t len) {
#ifdef EDGEGATE_HAVE_NGHTTP2
    if (!h2c) return;
    const ssize_t rv = nghttp2_session_mem_recv(h2c, data, len);
    if (rv < 0) {
        h2_failed = true;
        h2_error = "nghttp2_session_mem_recv failed";
        return;
    }
    // 释放已结束 stream 的 body holder
    if (h2_stream_id > 0 && h2_body_done) {
        void* ud = nghttp2_session_get_stream_user_data(h2c, h2_stream_id);
        if (ud) {
            delete static_cast<std::string*>(ud);
            nghttp2_session_set_stream_user_data(h2c, h2_stream_id, nullptr);
        }
    }
#else
    (void)data;
    (void)len;
#endif
}

// ---------------------------------------------------------------------------
// ClientConn
// ---------------------------------------------------------------------------

void ProxyServer::Impl::ClientConn::start() {
    if (closed) return;
    if (tls_handshaking) {
        handle_tls_handshake(0);
        return;
    }
    update_events();
}

IoStatus ProxyServer::Impl::ClientConn::client_read(void* buffer, std::size_t size,
                                                    std::size_t& got) {
    got = 0;
    if (tls) return tls->read(buffer, size, got);
    for (;;) {
        const ssize_t n = ::recv(fd, buffer, size, 0);
        if (n > 0) {
            got = static_cast<std::size_t>(n);
            return IoStatus::Ok;
        }
        if (n == 0) return IoStatus::Eof;
        if (errno == EINTR) continue;
        if (eagain(errno)) return IoStatus::WantRead;
        return IoStatus::Error;
    }
}

IoStatus ProxyServer::Impl::ClientConn::client_write(const void* buffer, std::size_t size,
                                                     std::size_t& wrote) {
    wrote = 0;
    if (tls) return tls->write(buffer, size, wrote);
    for (;;) {
        const ssize_t n = ::send(fd, buffer, size, MSG_NOSIGNAL);
        if (n > 0) {
            wrote = static_cast<std::size_t>(n);
            return IoStatus::Ok;
        }
        if (n == 0) return IoStatus::Error;
        if (errno == EINTR) continue;
        if (eagain(errno)) return IoStatus::WantWrite;
        return IoStatus::Error;
    }
}

void ProxyServer::Impl::ClientConn::handle_tls_handshake(std::uint32_t ev) {
    if (!tls || !tls_handshaking || closed) return;
    const IoStatus st = tls->handshake();
    if (st == IoStatus::Ok) {
        tls_handshaking = false;
        tls_want_write = false;
        last_activity_ms = now_ms();
        const std::string alpn = tls->alpn();
        log_msg(LogLevel::Debug, "TLS handshake complete from ", client_ip, " ALPN=",
                alpn.empty() ? "(none)" : alpn);
        if (alpn == "h2" && server->cfg.http2) {
            start_h2();
            if (h2_mode && h2) {
                h2->on_event(ev);
                if (close_requested) close_now();
                return;
            }
        }
        update_events();
        if (ev & EPOLLIN) on_readable();
        if (!closed && (ev & EPOLLOUT)) on_writable();
        return;
    }
    if (st == IoStatus::WantRead) {
        tls_want_write = false;
        update_events();
        return;
    }
    if (st == IoStatus::WantWrite) {
        tls_want_write = true;
        update_events();
        return;
    }
    log_msg(LogLevel::Debug, "TLS handshake failed from ", client_ip, ": ", tls_last_error());
    close_now();
}

void ProxyServer::Impl::ClientConn::on_event(int event_fd, std::uint32_t ev) {
    (void)event_fd;
    if (closed) return;
    if (tls_handshaking) {
        if (ev & EPOLLERR) {
            close_now();
            return;
        }
        handle_tls_handshake(ev);
        return;
    }
    if (h2_mode) {
        if (h2) h2->on_event(ev);
        if (close_requested) close_now();
        return;
    }
    if (ev & EPOLLERR) {
        close_now();
        return;
    }
    if (ev & EPOLLIN) {
        on_readable();
        if (closed) return;
    }
    if (ev & EPOLLOUT) {
        on_writable();
        if (closed) return;
    }
    if (ev & (EPOLLHUP | EPOLLRDHUP)) {
        on_client_eof();
    }
}

bool ProxyServer::Impl::ClientConn::read_allowed() const {
    if (closed || client_eof || client_paused) return false;
    if (state == State::Tunneling) return true;
    if (state == State::ReadRequest) return out.empty();
    if (state == State::SendingBody) return !req_body_done;
    return false;
}

void ProxyServer::Impl::ClientConn::update_events() {
    if (fd < 0 || closed) return;
    if (h2_mode) {
        if (h2) h2->update_events();
        return;
    }
    std::uint32_t want = EPOLLRDHUP;
    if (tls_handshaking) {
        want |= tls_want_write ? EPOLLOUT : EPOLLIN;
        if (want != events) {
            events = want;
            server->loop.mod(fd, want);
        }
        return;
    }
    if (read_allowed()) want |= EPOLLIN;
    if (io_want_read) want |= EPOLLIN;
    if (!out.empty() || io_want_write) want |= EPOLLOUT;
    if (want != events) {
        events = want;
        server->loop.mod(fd, want);
    }
}

void ProxyServer::Impl::ClientConn::set_events(std::uint32_t want) {
    if (fd < 0 || closed) return;
    if (want != events) {
        events = want;
        server->loop.mod(fd, want);
    }
}

void ProxyServer::Impl::ClientConn::set_client_paused(bool paused, bool count) {
    if (client_paused == paused) return;
    client_paused = paused;
    if (count) {
        if (paused) server->metrics.client_read_pauses.inc();
        else server->metrics.client_read_resumes.inc();
    }
    update_events();
}

void ProxyServer::Impl::ClientConn::set_upstream_paused(bool paused, bool count) {
    if (upstream_paused == paused) return;
    upstream_paused = paused;
    if (count) {
        if (paused) server->metrics.upstream_read_pauses.inc();
        else server->metrics.upstream_read_resumes.inc();
    }
    if (up) up->set_read_paused(paused);
}

void ProxyServer::Impl::ClientConn::maybe_resume_upstream() {
    if (!upstream_paused || !up) return;
    if (server->global_backpressure) return;
    if (out.size() < server->cfg.client_buffer_low && (!response_complete || tunnel_mode)) {
        set_upstream_paused(false, true);
    }
}

void ProxyServer::Impl::ClientConn::on_readable() {
    if (closed || tls_handshaking) return;
    if (h2_mode) {
        if (h2) h2->on_readable();
        if (close_requested) close_now();
        return;
    }
    if (tunnel_mode) {
        pump_tunnel_from_client();
        return;
    }
    io_want_read = false;
    char buf[kRecvChunk];
    for (;;) {
        if (!read_allowed()) break;
        const std::size_t cap = server->cfg.max_request_header_bytes + kRecvChunk;
        if (in.size() >= cap) break;
        std::size_t got = 0;
        const IoStatus st = client_read(buf, sizeof(buf), got);
        if (st == IoStatus::Ok && got > 0) {
            in.append(buf, got);
            last_activity_ms = now_ms();
            if (state == State::ReadRequest && !req_head_done && head_started_ms == 0) {
                head_started_ms = last_activity_ms;
            }
            handle_input();
            if (closed) return;
        } else if (st == IoStatus::WantRead) {
            break;
        } else if (st == IoStatus::WantWrite) {
            io_want_write = true;
            break;
        } else if (st == IoStatus::Eof) {
            on_client_eof();
            return;
        } else {
            server->metrics.read_errors.inc();
            close_now();
            return;
        }
    }
    update_events();
}

void ProxyServer::Impl::ClientConn::on_writable() {
    if (closed) return;
    if (h2_mode) {
        if (h2) h2->on_writable();
        if (close_requested) close_now();
        return;
    }
    flush_client_out();
    if (closed) return;
    if (out.empty()) {
        if (close_after_flush) {
            close_now();
            return;
        }
        if (state == State::Draining || state == State::ReadRequest) {
            state = State::ReadRequest;
            update_events();
            if (!in.empty()) handle_input();
            if (closed) return;
        }
    }
    update_events();
}

void ProxyServer::Impl::ClientConn::flush_client_out() {
    if (closed) return;
    io_want_write = false;
    while (!out.empty()) {
        std::size_t wrote = 0;
        const IoStatus st = client_write(out.data(), out.size(), wrote);
        if (st == IoStatus::Ok && wrote > 0) {
            out.consume(wrote);
            last_out_progress_ms = now_ms();
        } else if (st == IoStatus::WantWrite) {
            io_want_write = true;
            break;
        } else if (st == IoStatus::WantRead) {
            io_want_read = true;
            break;
        } else {
            server->metrics.write_errors.inc();
            close_now();
            return;
        }
    }
    if (out.size() < server->cfg.client_buffer_low) maybe_resume_upstream();
    update_events();
}

void ProxyServer::Impl::ClientConn::handle_input() {
    if (closed) return;
    if (state == State::ReadRequest) {
        if (req_head_done) return;
        HttpError err;
        const auto status = head_parser.parse(in, req, err);
        if (status == RequestHeadParser::Status::NeedMore) return;
        if (status == RequestHeadParser::Status::Error) {
            server->metrics.parse_errors.inc();
            send_error(err.status, err.message);
            return;
        }

        req_head_done = true;
        req_body_mode = head_parser.body_mode();
        req_body_remaining = req.content_length;
        head_started_ms = 0;
        request_started_ms = now_ms();
        request_sent_ms = 0;
        keep_client_alive = req.keep_alive && !client_eof;
        request_id = sanitize_request_id(header_value(req.headers, "x-request-id"));
        if (request_id.empty()) request_id = "eg-" + random_hex(8);
        server->metrics.requests_total.inc();
        if (req.http10) server->metrics.http10_requests.inc();
        if (req.chunked) server->metrics.chunked_requests.inc();
        start_trace();

        if (iequals(req.method, "CONNECT")) {
            connect_tunnel = true;
            websocket_upgrade = false;
        } else if (req.upgrade) {
            const std::string upg = header_value(req.headers, "upgrade");
            if (!iequals(upg, "websocket")) {
                send_error(501, "only websocket upgrades are supported");
                return;
            }
            websocket_upgrade = true;
            connect_tunnel = false;
        }
        if (server->cfg.max_request_body_bytes > 0 && req.has_content_length &&
            req.content_length > server->cfg.max_request_body_bytes) {
            send_error(413, "request body exceeds max_request_body_bytes");
            return;
        }

        if (req.expect_continue && !websocket_upgrade && !connect_tunnel) {
            out.append("HTTP/1.1 100 Continue\r\n\r\n");
            sent_100 = true;
        }

        state = State::Dispatching;
        set_client_paused(true, false);
        dispatch();
        return;
    }

    if (state == State::SendingBody) {
        pump_request_body();
    }
}

void ProxyServer::Impl::ClientConn::dispatch() {
    attempts = 0;
    tried.clear();
    replay_pending = false;
    try_attempt();
}

void ProxyServer::Impl::ClientConn::try_attempt() {
    for (;;) {
        if (attempts >= server->cfg.max_attempts) {
            send_error(502, "all upstream attempts failed");
            return;
        }
        Backend* b = server->select_backend(tried);
        if (!b) {
            if (attempts == 0) {
                server->metrics.no_backend.inc();
                send_error(503, "no healthy backend available");
            } else {
                send_error(502, "all upstream attempts failed");
            }
            return;
        }
        tried.push_back(b);
        backend = b;
        ++attempts;
        b->metrics.requests.inc();
        server->metrics.upstream_connections_total.inc();

        std::string why;
        bool connect_failed = false;
        bool pool_full = false;
        UpstreamConn* candidate =
            server->acquire_upstream(b, this, why, connect_failed, pool_full);
        if (!candidate) {
            if (connect_failed) {
                server->metrics.connect_errors.inc();
                b->metrics.errors.inc();
                continue;
            }
            if (pool_full) continue;
            send_error(502, why.empty() ? "cannot connect upstream" : why);
            return;
        }

        up = candidate;
        if (up->connecting) {
            state = State::Connecting;
            connect_start_ms = now_ms();
            set_client_paused(true, false);
            update_events();
            up->update_events();
            return;
        }
        connect_start_ms = now_ms();
        on_upstream_ready();
        return;
    }
}

void ProxyServer::Impl::ClientConn::on_upstream_ready() {
    if (closed || !up) return;
    const std::int64_t now = now_ms();
    connect_ms = connect_start_ms ? static_cast<double>(now - connect_start_ms) : 0.0;
    server->metrics.upstream_connect_ms.observe(connect_ms);
    backend->record_success(now);
    up->connected = true;
    up->connecting = false;
    up->update_events();

    // CONNECT：TCP 建连成功后直接回 200，进入双向隧道，不再向后端发 HTTP。
    if (connect_tunnel) {
        std::string head = "HTTP/1.1 200 Connection Established\r\n";
        head += "Via: 1.1 edgegate\r\n";
        head += "X-EdgeGate-Upstream: " + backend->name() + "\r\n";
        head += "X-EdgeGate-Request-Id: " + request_id + "\r\n\r\n";
        out.append(head);
        response_status = 200;
        response_started = true;
        resp_head_done = true;
        last_out_progress_ms = now;
        enter_tunnel("CONNECT tunnel established");
        return;
    }

    if (up->h2_mode) {
        if (websocket_upgrade) {
            on_upstream_failure("websocket upgrade cannot use HTTP/2 upstream", false, 502);
            return;
        }
        if (req_body_mode == BodyMode::None) req_body_done = true;
        if (!req_body_done) {
            // 先把 body 收进 replay_body，完成后再 h2_submit_request
            state = State::SendingBody;
            set_client_paused(false, false);
            update_events();
            pump_request_body();
            return;
        }
        std::string why;
        if (!up->h2_submit_request(req, replay_body, backend->endpoint().authority(), client_ip,
                                   request_id, server->cfg.preserve_host, why)) {
            on_upstream_failure(why.empty() ? "h2 upstream submit failed" : why, false, 502);
            return;
        }
        any_request_bytes_written = true;
        state = State::WaitingResponse;
        request_sent_ms = now;
        update_events();
        up->update_events();
        // 若响应已在 in 中（极少见），立刻处理
        if (!up->in.empty()) on_upstream_readable();
        return;
    }

    if (req_body_mode == BodyMode::None) req_body_done = true;
    const bool send_chunked = req.chunked && !websocket_upgrade;
    std::string head = build_request_head(req, backend->endpoint().authority(), client_ip,
                                          request_id, server->cfg.preserve_host, send_chunked,
                                          websocket_upgrade);
    up->out.append(head);
    up->update_events();

    if (replay_pending) {
        append_replay_body();
        req_body_done = true;
        replay_pending = false;
        state = State::WaitingResponse;
        request_sent_ms = now;
    } else if (req_body_done) {
        state = State::WaitingResponse;
        request_sent_ms = now;
    } else {
        state = State::SendingBody;
        set_client_paused(false, false);
    }
    update_events();
    if (state == State::SendingBody) pump_request_body();
}

void ProxyServer::Impl::ClientConn::capture_replay(std::string_view data) {
    if (!replay_ok) return;
    if (replay_body.size() + data.size() > server->cfg.retry_body_buffer_bytes) {
        replay_ok = false;
        replay_body.clear();
        return;
    }
    replay_body.append(data.data(), data.size());
}

void ProxyServer::Impl::ClientConn::queue_upstream_body(std::string_view data, bool frame_chunked) {
    if (!up) return;
    // h2 上游：body 只进 replay 缓冲，等完整后再 submit
    if (up->h2_mode) {
        body_forwarded = true;
        bytes_in += data.size();
        backend->metrics.bytes_in.inc(data.size());
        server->metrics.bytes_in_total.inc(data.size());
        capture_replay(data);
        return;
    }
    if (frame_chunked) up->out.append(chunk_frame(data));
    else up->out.append(data.data(), data.size());
    body_forwarded = true;
    bytes_in += data.size();
    backend->metrics.bytes_in.inc(data.size());
    server->metrics.bytes_in_total.inc(data.size());
    capture_replay(data);
    up->update_events();
}

void ProxyServer::Impl::ClientConn::append_replay_body() {
    if (!up) return;
    if (req.chunked) {
        if (!replay_body.empty()) up->out.append(chunk_frame(replay_body));
        up->out.append(last_chunk_frame());
    } else if (!replay_body.empty()) {
        up->out.append(replay_body);
    }
    up->update_events();
}

void ProxyServer::Impl::ClientConn::pump_request_body() {
    if (closed || !up || up->connecting || state != State::SendingBody) return;
    if (up->out.size() >= server->cfg.upstream_buffer_high) {
        set_client_paused(true, true);
        return;
    }

    if (server->cfg.max_request_body_bytes > 0 &&
        req_body_seen > server->cfg.max_request_body_bytes) {
        send_error(413, "request body exceeds max_request_body_bytes");
        return;
    }

    if (req_body_mode == BodyMode::Chunked) {
        for (;;) {
            std::string_view data;
            HttpError err;
            const auto st = request_chunks.next(in, data, err);
            if (st == ChunkedDecoder::Status::NeedMore) break;
            if (st == ChunkedDecoder::Status::Error) {
                send_error(400, err.message);
                return;
            }
            if (st == ChunkedDecoder::Status::Done) {
                if (up && up->h2_mode) {
                    std::string why;
                    if (!up->h2_submit_request(req, replay_body, backend->endpoint().authority(),
                                               client_ip, request_id, server->cfg.preserve_host,
                                               why)) {
                        on_upstream_failure(why.empty() ? "h2 upstream submit failed" : why, false,
                                            502);
                        return;
                    }
                    any_request_bytes_written = true;
                } else if (up) {
                    up->out.append(last_chunk_frame());
                }
                req_body_done = true;
                state = State::WaitingResponse;
                request_sent_ms = now_ms();
                set_client_paused(false, false);
                if (up) up->update_events();
                update_events();
                return;
            }
            req_body_seen += data.size();
            if (server->cfg.max_request_body_bytes > 0 &&
                req_body_seen > server->cfg.max_request_body_bytes) {
                send_error(413, "request body exceeds max_request_body_bytes");
                return;
            }
            queue_upstream_body(data, true);
            if (!up) return;
            if (up->out.size() >= server->cfg.upstream_buffer_high) {
                set_client_paused(true, true);
                return;
            }
        }
        if (up && up->out.size() < server->cfg.upstream_buffer_low) set_client_paused(false, false);
        if (up) up->update_events();
        update_events();
        return;
    }

    if (req_body_mode == BodyMode::ContentLength) {
        while (req_body_remaining > 0 && !in.empty()) {
            const std::size_t take = static_cast<std::size_t>(
                std::min<std::uint64_t>(req_body_remaining, in.size()));
            const std::string_view view(in.view().substr(0, take));
            req_body_remaining -= take;
            req_body_seen += take;
            in.consume(take);
            queue_upstream_body(view, false);
            if (!up) return;
            if (up->out.size() >= server->cfg.upstream_buffer_high) {
                set_client_paused(true, true);
                return;
            }
        }
        if (req_body_remaining == 0) {
            if (up && up->h2_mode) {
                std::string why;
                if (!up->h2_submit_request(req, replay_body, backend->endpoint().authority(),
                                           client_ip, request_id, server->cfg.preserve_host, why)) {
                    on_upstream_failure(why.empty() ? "h2 upstream submit failed" : why, false, 502);
                    return;
                }
                any_request_bytes_written = true;
            }
            req_body_done = true;
            state = State::WaitingResponse;
            request_sent_ms = now_ms();
            set_client_paused(false, false);
            if (up) up->update_events();
            update_events();
            return;
        }
        if (up && up->out.size() < server->cfg.upstream_buffer_low) set_client_paused(false, false);
        if (up) up->update_events();
        update_events();
    }
}

void ProxyServer::Impl::ClientConn::on_upstream_writable() {
    if (closed || !up) return;
    flush_upstream_out();
    if (closed || !up) return;
    if (up->out.size() < server->cfg.upstream_buffer_low) {
        if (req_body_mode != BodyMode::None && !req_body_done) {
            set_client_paused(false, false);
            pump_request_body();
            if (closed || !up) return;
        }
    }
    if (state == State::SendingBody && req_body_done) {
        state = State::WaitingResponse;
        request_sent_ms = now_ms();
    }
    up->update_events();
    update_events();
}

void ProxyServer::Impl::ClientConn::flush_upstream_out() {
    if (!up) return;
    while (!up->out.empty()) {
        const ssize_t n = ::send(up->fd, up->out.data(), up->out.size(), MSG_NOSIGNAL);
        if (n > 0) {
            any_request_bytes_written = true;
            up->out.consume(static_cast<std::size_t>(n));
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else if (n < 0 && eagain(errno)) {
            break;
        } else {
            on_upstream_failure(std::string("upstream write: ") + std::strerror(errno), false, 502);
            return;
        }
    }
    if (up) up->update_events();
}

void ProxyServer::Impl::ClientConn::on_upstream_readable() {
    if (closed || !up || up->connecting) return;
    if (tunnel_mode) {
        pump_tunnel_from_upstream();
        return;
    }
    if (up->h2_mode) {
        if (up->h2_failed) {
            on_upstream_failure(up->h2_error.empty() ? "upstream h2 failed" : up->h2_error, false, 502);
            return;
        }
        // h2_pump_read 可能已在 on_event 里调用；这里消费 in
        process_upstream_input();
        if (closed || !up) return;
        if (up->h2_body_done && up->in.empty()) {
            if (!resp_head_done) {
                on_upstream_failure("upstream h2 closed before response", false, 502);
                return;
            }
            if (resp_body_mode == BodyMode::UntilClose || !response_complete) {
                on_upstream_eof();
                return;
            }
        }
        if (up) up->update_events();
        return;
    }
    if (server->global_backpressure && out.size() > server->cfg.client_buffer_low) {
        set_upstream_paused(true, true);
        return;
    }
    if (out.size() >= server->cfg.client_buffer_high) {
        set_upstream_paused(true, true);
        return;
    }

    char buf[kRecvChunk];
    for (;;) {
        const ssize_t n = ::recv(up->fd, buf, sizeof(buf), 0);
        if (n > 0) {
            up->in.append(buf, static_cast<std::size_t>(n));
            last_activity_ms = now_ms();
            process_upstream_input();
            if (closed || !up) return;
            if (upstream_paused) return;
            if (out.size() >= server->cfg.client_buffer_high) {
                set_upstream_paused(true, true);
                return;
            }
            continue;
        }
        if (n == 0) {
            on_upstream_eof();
            return;
        }
        if (errno == EINTR) continue;
        if (eagain(errno)) break;
        server->metrics.read_errors.inc();
        on_upstream_failure(std::string("upstream read: ") + std::strerror(errno), false, 502);
        return;
    }
    up->update_events();
}

void ProxyServer::Impl::ClientConn::on_upstream_eof() {
    if (closed || !up) return;
    if (resp_head_done && resp_body_mode == BodyMode::UntilClose) {
        finish_response();
        return;
    }
    if (!response_started) {
        on_upstream_failure("upstream closed before response", false, 502);
        return;
    }
    server->metrics.upstream_eof.inc();
    server->metrics.read_errors.inc();
    close_now();
}

void ProxyServer::Impl::ClientConn::process_upstream_input() {
    for (;;) {
        if (closed || !up) return;

        if (!resp_head_done) {
            HttpError err;
            const bool is_head = iequals(req.method, "HEAD");
            const auto st = resp_parser.parse(up->in, is_head, resp, err);
            if (st == ResponseHeadParser::Status::NeedMore) return;
            if (st == ResponseHeadParser::Status::Error) {
                on_upstream_failure(err.message, false, 502);
                return;
            }

            if (resp.status >= 100 && resp.status < 200 && resp.status != 101) {
                const bool duplicate_100 = resp.status == 100 && sent_100;
                if (!duplicate_100) {
                    const std::string interim =
                        build_response_head(resp, backend->name(), request_id, req.http10, true, false);
                    out.append(interim);
                    last_out_progress_ms = now_ms();
                    update_events();
                }
                resp_parser.reset();
                continue;
            }

            if (!req_body_done) {
                request_body_aborted = true;
                close_upstream_after_response = true;
                keep_client_alive = false;
                if (up) {
                    up->out.clear();
                    up->update_events();
                }
            }

            resp_head_done = true;
            response_started = true;
            response_status = resp.status;
            trace.status = resp.status;
            const std::int64_t now = now_ms();
            ttfb_ms = static_cast<double>(now - request_started_ms);
            server->metrics.ttfb_ms.observe(ttfb_ms);
            backend->metrics.ttfb_ms.observe(ttfb_ms);
            backend->record_response(now, resp.status);

            resp_body_mode = resp_parser.body_mode();
            resp_remaining = resp.content_length;

            const bool no_body = resp_body_mode == BodyMode::None;
            client_chunked_response =
                !req.http10 && !no_body && !resp.has_content_length;
            const bool close_delimited =
                !no_body && !resp.has_content_length && !client_chunked_response;
            if (close_delimited) keep_client_alive = false;

            if (resp.status == 101) {
                if (!websocket_upgrade) {
                    on_upstream_failure("unexpected 101 Switching Protocols", false, 502);
                    return;
                }
                keep_client_alive = false;
                const std::string head = build_response_head(resp, backend->name(), request_id,
                                                             req.http10, false, false, true);
                out.append(head);
                last_out_progress_ms = now_ms();
                if (resp.chunked) server->metrics.chunked_responses.inc();
                trace.chunked_response = resp.chunked;
                if (!up->in.empty()) {
                    out.append(up->in.view());
                    bytes_out += up->in.size();
                    up->in.clear();
                }
                update_events();
                enter_tunnel("websocket upgrade");
                return;
            }

            const std::string head = build_response_head(resp, backend->name(), request_id,
                                                         req.http10, keep_client_alive,
                                                         client_chunked_response);
            out.append(head);
            last_out_progress_ms = now_ms();
            if (resp.chunked) server->metrics.chunked_responses.inc();
            trace.chunked_response = resp.chunked;
            update_events();

            if (no_body) {
                state = State::StreamingResponse;
                finish_response();
                return;
            }
            state = State::StreamingResponse;
        }

        if (resp_body_mode == BodyMode::ContentLength) {
            if (resp_remaining == 0) {
                finish_response();
                return;
            }
            if (up->in.empty()) return;
            const std::size_t take = static_cast<std::size_t>(
                std::min<std::uint64_t>(resp_remaining, up->in.size()));
            const std::string_view view(up->in.view().substr(0, take));
            up->in.consume(take);
            resp_remaining -= take;
            write_client_body(view);
            if (resp_remaining == 0) {
                finish_response();
                return;
            }
        } else if (resp_body_mode == BodyMode::Chunked) {
            std::string_view data;
            HttpError err;
            const auto st = resp_chunks.next(up->in, data, err);
            if (st == ChunkedDecoder::Status::NeedMore) return;
            if (st == ChunkedDecoder::Status::Error) {
                server->metrics.read_errors.inc();
                close_now();
                return;
            }
            if (st == ChunkedDecoder::Status::Done) {
                finish_response();
                return;
            }
            write_client_body(data);
        } else if (resp_body_mode == BodyMode::UntilClose) {
            if (!up->in.empty()) {
                const std::string_view view = up->in.view();
                write_client_body(view);
                up->in.clear();
            }
            if (out.size() >= server->cfg.client_buffer_high) {
                set_upstream_paused(true, true);
                return;
            }
            return;
        } else {
            finish_response();
            return;
        }

        if (out.size() >= server->cfg.client_buffer_high) {
            set_upstream_paused(true, true);
            return;
        }
    }
}

void ProxyServer::Impl::ClientConn::write_client_body(std::string_view data) {
    if (client_chunked_response) out.append(chunk_frame(data));
    else out.append(data.data(), data.size());
    bytes_out += data.size();
    backend->metrics.bytes_out.inc(data.size());
    server->metrics.bytes_out_total.inc(data.size());
    last_out_progress_ms = now_ms();
    update_events();
    if (out.size() >= server->cfg.client_buffer_high) set_upstream_paused(true, true);
    else if (upstream_paused && out.size() < server->cfg.client_buffer_low) {
        set_upstream_paused(false, true);
    }
}

void ProxyServer::Impl::ClientConn::enter_tunnel(const char* reason) {
    tunnel_mode = true;
    state = State::Tunneling;
    keep_client_alive = false;
    close_after_flush = false;
    response_complete = false;
    req_body_done = true;
    set_client_paused(false, false);
    set_upstream_paused(false, false);
    if (up) {
        up->read_paused = false;
        up->update_events();
    }
    log_msg(LogLevel::Info, "tunnel mode from ", client_ip, " via ",
            backend ? backend->name() : "?", ": ", reason ? reason : "");
    // 提交一次 trace（隧道随后的字节计入 bytes_in/out，结束时再更新）
    if (!trace_committed) {
        trace.status = response_status ? response_status : 101;
        trace.ttfb_ms = ttfb_ms;
        trace.connect_ms = connect_ms;
        // 隧道结束时再 commit；这里只标记 response 已开始
    }
    update_events();
    flush_client_out();
    if (closed) return;
    flush_upstream_out();
    if (!in.empty()) pump_tunnel_from_client();
}

void ProxyServer::Impl::ClientConn::pump_tunnel_from_client() {
    if (closed || !tunnel_mode || !up) return;
    // 把客户端 in 缓冲写入上游 out
    if (!in.empty()) {
        up->out.append(in.view());
        bytes_in += in.size();
        if (backend) backend->metrics.bytes_in.inc(in.size());
        server->metrics.bytes_in_total.inc(in.size());
        in.clear();
        flush_upstream_out();
        if (closed) return;
    }
    if (up && up->out.size() >= server->cfg.upstream_buffer_high) {
        set_client_paused(true, true);
    } else {
        set_client_paused(false, false);
    }
    // 继续从客户端 socket 读
    char buf[kRecvChunk];
    for (;;) {
        if (client_paused || closed) break;
        if (up && up->out.size() >= server->cfg.upstream_buffer_high) {
            set_client_paused(true, true);
            break;
        }
        std::size_t got = 0;
        const IoStatus st = client_read(buf, sizeof(buf), got);
        if (st == IoStatus::Ok && got > 0) {
            last_activity_ms = now_ms();
            up->out.append(buf, got);
            bytes_in += got;
            if (backend) backend->metrics.bytes_in.inc(got);
            server->metrics.bytes_in_total.inc(got);
            flush_upstream_out();
            if (closed) return;
            continue;
        }
        if (st == IoStatus::WantRead) break;
        if (st == IoStatus::WantWrite) {
            io_want_write = true;
            break;
        }
        if (st == IoStatus::Eof) {
            // 客户端半关闭：排空上游写缓冲后关闭
            client_eof = true;
            if (up && up->out.empty()) close_now();
            else update_events();
            return;
        }
        close_now();
        return;
    }
    update_events();
}

void ProxyServer::Impl::ClientConn::pump_tunnel_from_upstream() {
    if (closed || !tunnel_mode || !up) return;
    if (out.size() >= server->cfg.client_buffer_high) {
        set_upstream_paused(true, true);
        return;
    }
    char buf[kRecvChunk];
    for (;;) {
        if (upstream_paused || closed || !up) break;
        const ssize_t n = ::recv(up->fd, buf, sizeof(buf), 0);
        if (n > 0) {
            last_activity_ms = now_ms();
            out.append(buf, static_cast<std::size_t>(n));
            bytes_out += static_cast<std::uint64_t>(n);
            if (backend) backend->metrics.bytes_out.inc(static_cast<std::uint64_t>(n));
            server->metrics.bytes_out_total.inc(static_cast<std::uint64_t>(n));
            flush_client_out();
            if (closed) return;
            if (out.size() >= server->cfg.client_buffer_high) {
                set_upstream_paused(true, true);
                break;
            }
            continue;
        }
        if (n == 0) {
            // 上游关闭：排空客户端写缓冲后结束
            if (out.empty()) close_now();
            else {
                close_after_flush = true;
                if (up) {
                    server->destroy_upstream(up, false);
                    up = nullptr;
                }
                update_events();
            }
            return;
        }
        if (errno == EINTR) continue;
        if (eagain(errno)) break;
        close_now();
        return;
    }
    if (up) up->update_events();
    update_events();
}

void ProxyServer::Impl::ClientConn::finish_response() {
    if (response_complete) return;
    response_complete = true;
    if (client_chunked_response && resp.status != 204 && resp.status != 304) {
        out.append(last_chunk_frame());
    }
    const std::int64_t now = now_ms();
    const double duration = static_cast<double>(now - request_started_ms);
    server->metrics.request_duration_ms.observe(duration);
    server->metrics.responses_total.inc();
    if (backend) backend->metrics.latency_ms.observe(duration);

    const bool reusable = up && up->connected && resp.keep_alive && resp_body_mode != BodyMode::UntilClose &&
                          resp.status != 101 && !request_body_aborted &&
                          !close_upstream_after_response && up->in.empty();
    if (up) {
        if (reusable) server->release_upstream(up, true);
        else server->destroy_upstream(up, false);
        up = nullptr;
    }

    trace.duration_ms = duration;
    trace.ttfb_ms = ttfb_ms;
    trace.connect_ms = connect_ms;
    trace.bytes_in = bytes_in;
    trace.bytes_out = bytes_out;
    trace.attempts = attempts;
    trace.backend = backend ? backend->name() : "";
    trace.status = response_status;
    commit_trace();

    const bool keep = keep_client_alive && !client_eof;
    reset_for_next();
    keep_client_alive = keep;
    if (!keep) close_after_flush = true;
    state = out.empty() ? State::ReadRequest : State::Draining;
    update_events();

    if (out.empty()) {
        if (close_after_flush) {
            close_now();
            return;
        }
        if (!in.empty()) handle_input();
    }
}

void ProxyServer::Impl::ClientConn::reset_for_next() {
    req = RequestHead{};
    head_parser.reset();
    request_chunks.reset();
    req_body_mode = BodyMode::None;
    req_body_remaining = 0;
    req_body_seen = 0;
    req_head_done = false;
    req_body_done = false;
    sent_100 = false;
    request_id.clear();
    backend = nullptr;
    tried.clear();
    attempts = 0;
    any_request_bytes_written = false;
    body_forwarded = false;
    replay_ok = true;
    replay_pending = false;
    replay_body.clear();
    resp_parser.reset();
    resp_chunks.reset();
    resp = ResponseHead{};
    resp_body_mode = BodyMode::None;
    resp_remaining = 0;
    resp_head_done = false;
    response_started = false;
    response_complete = false;
    client_chunked_response = false;
    close_upstream_after_response = false;
    request_body_aborted = false;
    close_after_flush = false;
    client_paused = false;
    upstream_paused = false;
    request_active = false;
    trace_committed = false;
    websocket_upgrade = false;
    connect_tunnel = false;
    tunnel_mode = false;
    request_started_ms = 0;
    request_sent_ms = 0;
    connect_start_ms = 0;
    connect_ms = 0;
    ttfb_ms = 0;
    response_status = 0;
    bytes_in = 0;
    bytes_out = 0;
    trace = RequestTrace{};
    head_started_ms = 0;
    last_activity_ms = now_ms();
    last_out_progress_ms = last_activity_ms;
}

void ProxyServer::Impl::ClientConn::start_trace() {
    trace = RequestTrace{};
    trace.id = request_id;
    trace.method = req.method;
    trace.target = req.target;
    trace.client_ip = client_ip;
    trace.start_unix_ms = unix_ms();
    trace.keep_alive = keep_client_alive;
    trace.chunked_request = req.chunked;
    request_active = true;
    trace_committed = false;
}

void ProxyServer::Impl::ClientConn::commit_trace(std::string_view error) {
    if (!request_active || trace_committed) return;
    trace_committed = true;
    request_active = false;
    if (!error.empty()) trace.error = std::string(error);
    trace.duration_ms = static_cast<double>(now_ms() - request_started_ms);
    trace.ttfb_ms = ttfb_ms;
    trace.connect_ms = connect_ms;
    trace.bytes_in = bytes_in;
    trace.bytes_out = bytes_out;
    trace.attempts = attempts;
    trace.backend = backend ? backend->name() : "";
    if (trace.status == 0) trace.status = response_status;
    server->commit_trace(trace);
}

void ProxyServer::Impl::ClientConn::send_error(int status, std::string_view message) {
    if (closed) return;
    if (response_started) {
        close_now();
        return;
    }
    if (up) {
        server->destroy_upstream(up, true);
        up = nullptr;
    }
    server->metrics.requests_rejected.inc();
    if (status == 413) server->metrics.body_too_large.inc();
    if (!request_active) {
        trace.status = status;
        trace.error = std::string(message);
    } else {
        trace.status = status;
        commit_trace(message);
    }
    const std::string body =
        build_error_response(status, message, request_id, false, req.http10,
                             iequals(req.method, "HEAD"));
    out.append(body);
    keep_client_alive = false;
    close_after_flush = true;
    state = State::Draining;
    set_client_paused(true, false);
    last_out_progress_ms = now_ms();
    update_events();
    if (out.empty()) close_now();
}

void ProxyServer::Impl::ClientConn::close_now() {
    if (closed) return;
    closed = true;
    if (request_active || tunnel_mode) {
        if (tunnel_mode) {
            const std::int64_t now = now_ms();
            trace.duration_ms = request_started_ms ? static_cast<double>(now - request_started_ms) : 0;
            trace.bytes_in = bytes_in;
            trace.bytes_out = bytes_out;
            trace.status = response_status ? response_status : (connect_tunnel ? 200 : 101);
            if (backend) backend->metrics.latency_ms.observe(trace.duration_ms);
            server->metrics.request_duration_ms.observe(trace.duration_ms);
            if (!trace_committed) server->metrics.responses_total.inc();
        }
        trace.status = response_status ? response_status : trace.status;
        commit_trace(tunnel_mode ? "tunnel_closed" : "client_closed");
    }
    if (up) {
        up->owner = nullptr;
        server->destroy_upstream(up, true);
        up = nullptr;
    }
    if (h2) {
        h2->shutdown();
        h2.reset();
        h2_mode = false;
    }
    if (tls) tls->shutdown();
    if (fd >= 0) {
        server->loop.close_later(fd);
        server->zombie_clients.push_back(key_fd);
        fd = -1;
    }
    server->metrics.client_disconnects.inc();
    server->metrics.active_clients.dec();
}

void ProxyServer::Impl::ClientConn::on_client_eof() {
    if (closed) return;
    if (h2_mode) {
        if (h2) h2->on_client_eof();
        if (close_requested) close_now();
        return;
    }
    client_eof = true;
    set_client_paused(true, false);
    if (tunnel_mode) {
        // 客户端半关闭：尽量把上游写缓冲排空再关
        if (up) flush_upstream_out();
        if (!up || up->out.empty()) close_now();
        else update_events();
        return;
    }
    if (state == State::ReadRequest || (state == State::SendingBody && !req_body_done)) {
        close_now();
        return;
    }
    keep_client_alive = false;
    close_after_flush = true;
    if (!request_active && out.empty()) close_now();
}

void ProxyServer::Impl::ClientConn::on_tick(std::int64_t now) {
    if (closed) return;
    if (h2_mode) {
        if (h2) h2->on_tick(now);
        if (close_requested) close_now();
        return;
    }
    if (tls_handshaking) {
        if (now - tls_started_ms > server->cfg.tls_handshake_timeout_ms) {
            log_msg(LogLevel::Debug, "TLS handshake timeout from ", client_ip);
            close_now();
        }
        return;
    }
    if (!out.empty() && now - last_out_progress_ms > server->cfg.client_write_stall_ms) {
        log_msg(LogLevel::Warn, "client ", client_ip, " write stalled for ",
                now - last_out_progress_ms, " ms; closing");
        close_now();
        return;
    }
    if (state == State::ReadRequest && !req_head_done && in.empty() && !request_active &&
        now - last_activity_ms > server->cfg.client_idle_timeout_ms) {
        close_now();
        return;
    }
    if (!req_head_done && head_started_ms > 0 &&
        now - head_started_ms > server->cfg.client_header_timeout_ms) {
        send_error(408, "request header timeout");
        return;
    }
    if (state == State::SendingBody &&
        now - last_activity_ms > server->cfg.client_idle_timeout_ms) {
        send_error(408, "client body idle timeout");
        return;
    }
    if (state == State::Connecting && now - connect_start_ms > server->cfg.upstream_connect_timeout_ms) {
        on_upstream_failure("upstream connect timeout", true, 504);
        return;
    }
    if (state == State::WaitingResponse && request_sent_ms > 0 &&
        now - request_sent_ms > server->cfg.upstream_response_timeout_ms) {
        on_upstream_failure("upstream response timeout", false, 504);
        return;
    }
    if (request_active && !response_started &&
        now - request_started_ms > server->cfg.request_timeout_ms) {
        on_upstream_failure("request timeout", false, 504);
        return;
    }
}

bool ProxyServer::Impl::ClientConn::can_retry_now() const {
    RetryInput ri;
    ri.method = req.method;
    ri.attempts_done = attempts;
    ri.max_attempts = server->cfg.max_attempts;
    ri.idempotent_only = server->cfg.retry_idempotent_only;
    ri.request_bytes_written = any_request_bytes_written;
    ri.body_forwarded = body_forwarded;
    ri.body_complete = req_body_done;
    ri.body_replayable = replay_ok;
    ri.response_started = response_started;
    return can_retry(ri);
}

void ProxyServer::Impl::ClientConn::on_upstream_failure(std::string reason, bool connect_failure,
                                                        int status_hint) {
    if (closed) return;
    const bool was_connecting = up && up->connecting;
    const bool timeout = reason.find("timeout") != std::string::npos;
    if (up) {
        server->destroy_upstream(up, true);
        up = nullptr;
    }
    if (backend) {
        backend->metrics.errors.inc();
        backend->record_failure(now_ms(), connect_failure || was_connecting);
    }
    if (timeout) server->metrics.timeouts.inc();
    else if (connect_failure || was_connecting) server->metrics.connect_errors.inc();
    else server->metrics.read_errors.inc();

    if (response_started) {
        commit_trace(reason);
        close_now();
        return;
    }

    if (can_retry_now()) {
        server->metrics.retries.inc();
        if (backend) backend->metrics.retries.inc();
        trace.retried = true;
        resp_parser.reset();
        resp_chunks.reset();
        resp = ResponseHead{};
        resp_head_done = false;
        response_started = false;
        response_complete = false;
        resp_body_mode = BodyMode::None;
        resp_remaining = 0;
        client_chunked_response = false;
        close_upstream_after_response = false;
        request_body_aborted = false;
        replay_pending = body_forwarded;
        state = State::Dispatching;
        set_client_paused(true, false);
        try_attempt();
        return;
    }
    send_error(status_hint, reason);
}

// ---------------------------------------------------------------------------
// HealthConn
// ---------------------------------------------------------------------------

void ProxyServer::Impl::HealthConn::update_events() {
    if (fd < 0 || dead) return;
    std::uint32_t want = EPOLLRDHUP;
    if (connecting) want |= EPOLLOUT;
    else if (reading) want |= EPOLLIN;
    if (want != events) {
        events = want;
        server->loop.mod(fd, want);
    }
}

void ProxyServer::Impl::HealthConn::send_request() {
    connecting = false;
    reading = true;
    request = "GET " + server->cfg.health_path + " HTTP/1.1\r\nHost: " +
              backend->endpoint().authority() +
              "\r\nUser-Agent: edgegate-health\r\nConnection: close\r\n\r\n";
    const ssize_t n = ::send(fd, request.data(), request.size(), MSG_NOSIGNAL);
    if (n < 0 && !eagain(errno)) {
        finish(false, "health request write failed");
        return;
    }
    update_events();
}

void ProxyServer::Impl::HealthConn::on_event(int event_fd, std::uint32_t ev) {
    (void)event_fd;
    if (dead || finished) return;
    if (connecting) {
        if (ev & (EPOLLERR | EPOLLHUP)) {
            finish(false, "health connect failed");
            return;
        }
        if (ev & EPOLLOUT) {
            int err = 0;
            socklen_t len = sizeof(err);
            if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) != 0 || err != 0) {
                finish(false, "health connect error");
                return;
            }
            send_request();
        }
        return;
    }
    if (ev & (EPOLLERR | EPOLLHUP)) {
        finish(false, "health socket error");
        return;
    }
    if (ev & EPOLLIN) {
        char buf[8192];
        for (;;) {
            const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
            if (n > 0) {
                in.append(buf, static_cast<std::size_t>(n));
                if (in.size() > 16384) {
                    finish(false, "health response too large");
                    return;
                }
                ResponseHead head;
                HttpError err;
                ResponseHeadParser parser(16384);
                const auto st = parser.parse(in, false, head, err);
                if (st == ResponseHeadParser::Status::NeedMore) continue;
                if (st == ResponseHeadParser::Status::Error) {
                    finish(false, err.message);
                    return;
                }
                finish(head.status >= 200 && head.status < 400,
                       "health status " + std::to_string(head.status));
                return;
            }
            if (n == 0) {
                finish(false, "health connection closed");
                return;
            }
            if (errno == EINTR) continue;
            if (eagain(errno)) break;
            finish(false, "health read failed");
            return;
        }
    }
    update_events();
}

void ProxyServer::Impl::HealthConn::on_tick(std::int64_t now) {
    if (dead || finished) return;
    if (now - start_ms > server->cfg.health_timeout_ms) {
        finish(false, "health check timeout");
    }
}

void ProxyServer::Impl::HealthConn::finish(bool ok, std::string_view why) {
    if (finished) return;
    finished = true;
    server->metrics.health_checks.inc();
    if (ok) {
        backend->record_success(now_ms());
    } else {
        server->metrics.health_failures.inc();
        backend->record_failure(now_ms(), false);
    }
    log_msg(LogLevel::Debug, "health ", backend->name(), ": ", why);
    server->destroy_health(this);
}

// ---------------------------------------------------------------------------
// Public facade
// ---------------------------------------------------------------------------

ProxyServer::ProxyServer(Config config) : impl_(std::make_unique<Impl>(std::move(config), this)) {}
ProxyServer::~ProxyServer() = default;

bool ProxyServer::init(std::string& error) { return impl_->init(error); }
int ProxyServer::run() { return impl_->run(); }
void ProxyServer::request_stop() { impl_->request_stop(); }

const Config& ProxyServer::config() const { return impl_->cfg; }
const Metrics& ProxyServer::metrics() const { return impl_->metrics; }
std::int64_t ProxyServer::uptime_seconds() const { return (now_ms() - impl_->start_ms) / 1000; }
std::vector<BackendSnapshot> ProxyServer::backend_snapshots() const { return impl_->snapshots(); }
std::string ProxyServer::prometheus() const {
    return render_prometheus(impl_->metrics, impl_->snapshots(), uptime_seconds());
}
std::string ProxyServer::status_json() const { return impl_->status_json(); }
std::string ProxyServer::recent_requests_json(std::size_t limit) const {
    return impl_->recent_requests_json(limit);
}
std::string ProxyServer::trace_json(std::string_view id) const { return impl_->trace_json(id); }
bool ProxyServer::set_backend_draining(std::string_view name, bool draining, std::string& error) {
    return impl_->set_backend_draining(name, draining, error);
}

}  // namespace eg
