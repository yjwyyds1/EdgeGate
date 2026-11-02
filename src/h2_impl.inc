// HTTP/2 frontend (nghttp2). Included by proxy.cpp after ClientConn/UpstreamConn
// declarations. Request bodies are buffered up to http2_max_request_body_bytes;
// responses are streamed through nghttp2's data provider with backpressure.

#ifndef EDGEGATE_HAVE_NGHTTP2

struct ProxyServer::Impl::H2Session {
    void shutdown() {}
    void on_event(std::uint32_t) {}
    void on_readable() {}
    void on_writable() {}
    void on_tick(std::int64_t) {}
    void on_client_eof() {}
    void update_events() {}
    std::size_t buffered_bytes() const { return 0; }
    std::size_t active_streams() const { return 0; }
};

#else

namespace {

struct H2NvStore {
    std::vector<std::string> names;
    std::vector<std::string> values;
    std::vector<nghttp2_nv> nv;

    void add(std::string name, std::string value) {
        names.push_back(std::move(name));
        values.push_back(std::move(value));
    }
    void build() {
        nv.clear();
        nv.reserve(names.size());
        for (std::size_t i = 0; i < names.size(); ++i) {
            nghttp2_nv entry{};
            entry.name = reinterpret_cast<uint8_t*>(names[i].data());
            entry.value = reinterpret_cast<uint8_t*>(values[i].data());
            entry.namelen = names[i].size();
            entry.valuelen = values[i].size();
            entry.flags = NGHTTP2_NV_FLAG_NONE;
            nv.push_back(entry);
        }
    }
};

bool h2_forbidden_header(std::string_view name) {
    return name == "connection" || name == "keep-alive" || name == "proxy-connection" ||
           name == "transfer-encoding" || name == "upgrade" || name == "te" ||
           name == "trailer";
}

}  // namespace

struct ProxyServer::Impl::H2Stream : UpstreamOwner {
    enum class State { Init, Connecting, Waiting, Streaming, Done, Closed };

    Impl* server = nullptr;
    H2Session* session = nullptr;
    ClientConn* client = nullptr;
    int32_t id = 0;
    std::string client_ip;
    std::string method, path, authority, scheme, request_id;
    std::vector<Header> headers;
    std::string body;
    std::uint64_t request_body_bytes = 0;
    bool body_done = false;
    bool too_large = false;
    bool closed = false;
    bool committed = false;
    State state = State::Init;

    Backend* backend = nullptr;
    UpstreamConn* up = nullptr;
    std::vector<Backend*> tried;
    int attempts = 0;

    RequestHead req;
    ResponseHeadParser parser;
    ChunkedDecoder chunks;
    ResponseHead resp;
    BodyMode resp_mode = BodyMode::None;
    std::uint64_t resp_remaining = 0;
    bool resp_head_done = false;
    bool response_started = false;
    bool response_headers_sent = false;
    bool response_complete = false;
    bool request_bytes_written = false;
    bool up_read_paused = false;
    bool data_deferred = false;
    bool provider_attached = false;

    std::string pending;
    std::size_t pending_off = 0;
    std::uint64_t response_bytes = 0;
    nghttp2_data_provider2 provider{};

    std::int64_t started_ms = 0;
    std::int64_t request_sent_ms = 0;
    std::int64_t last_activity_ms = 0;
    double ttfb_ms = 0;
    double connect_ms = 0;
    int status = 0;
    RequestTrace trace;

    H2Stream(Impl* s, H2Session* sess, ClientConn* c, int32_t stream_id)
        : server(s), session(sess), client(c), id(stream_id) {}
    ~H2Stream() override;

    void on_data(const std::uint8_t* data, std::size_t len);
    void on_end_stream();
    void dispatch();
    void try_attempt();
    void on_upstream_ready() override;
    void on_upstream_failure(std::string reason, bool connect_failure, int status_hint) override;
    void on_upstream_readable() override;
    void on_upstream_eof() override;
    void on_upstream_writable() override;
    void process_upstream_input();
    void submit_response_headers();
    void submit_error(int error_status, std::string message);
    void submit_rst(std::uint32_t code);
    void finish_response();
    void flush_request();
    void append_pending(std::string_view data);
    void set_up_paused(bool paused);
    void abort_upstream();
    void commit_trace(std::string_view error = {});
    void on_tick(std::int64_t now);
    void on_close();
    void start_trace();
    bool can_retry() const;
    std::size_t pending_bytes() const { return pending.size() - pending_off; }

    static nghttp2_ssize data_read(nghttp2_session*, int32_t, std::uint8_t* buf,
                                   std::size_t length, std::uint32_t* flags,
                                   nghttp2_data_source* source, void*);
    nghttp2_ssize read_data(std::uint8_t* buf, std::size_t length, std::uint32_t* flags);
};

struct ProxyServer::Impl::H2Session {
    Impl* server = nullptr;
    ClientConn* client = nullptr;
    nghttp2_session* ns = nullptr;
    std::unordered_map<int32_t, std::unique_ptr<H2Stream>> streams;
    std::vector<int32_t> erase_queue;
    Buffer out;
    std::size_t total_body = 0;
    bool closed = false;
    bool client_eof = false;
    bool need_write = false;
    bool read_paused = false;
    bool in_recv = false;
    bool flush_pending = false;
    std::int64_t last_activity_ms = 0;

    H2Session(Impl* s, ClientConn* c) : server(s), client(c) {}
    ~H2Session() { shutdown(); }

    bool init(std::string& error);
    void shutdown();
    void on_event(std::uint32_t ev);
    void on_readable();
    void on_writable();
    void on_tick(std::int64_t now);
    void on_client_eof();
    void update_events();
    std::uint32_t interest() const;
    void flush();
    void schedule_flush();
    void reap();
    void release_body(H2Stream* stream);
    void fail_connection();
    std::size_t buffered_bytes() const;
    std::size_t active_streams() const { return streams.size(); }

    static int on_begin_headers(nghttp2_session*, const nghttp2_frame*, void*);
    static int on_header(nghttp2_session*, const nghttp2_frame*, const std::uint8_t* name,
                         std::size_t namelen, const std::uint8_t* value, std::size_t valuelen,
                         std::uint8_t flags, void*);
    static int on_data_chunk(nghttp2_session*, std::uint8_t flags, int32_t stream_id,
                             const std::uint8_t* data, std::size_t len, void*);
    static int on_frame_recv(nghttp2_session*, const nghttp2_frame*, void*);
    static int on_stream_close(nghttp2_session*, int32_t stream_id, std::uint32_t error_code,
                               void*);
};

ProxyServer::Impl::H2Stream::~H2Stream() {
    abort_upstream();
    if (session) session->release_body(this);
}

// ---------------------------------------------------------------------------
// H2Stream
// ---------------------------------------------------------------------------

void ProxyServer::Impl::H2Stream::start_trace() {
    trace = RequestTrace{};
    trace.id = request_id;
    trace.method = method;
    trace.target = path;
    trace.client_ip = client_ip;
    trace.start_unix_ms = unix_ms();
    trace.keep_alive = true;
    trace.chunked_request = false;
    trace.chunked_response = false;
    server->metrics.requests_total.inc();
    server->metrics.active_requests.inc();
}

void ProxyServer::Impl::H2Stream::on_data(const std::uint8_t* data, std::size_t len) {
    if (closed || body_done || too_large) return;
    const std::size_t max_stream = server->cfg.http2_max_request_body_bytes;
    if (body.size() + len > max_stream ||
        session->total_body + len > server->cfg.http2_max_total_request_body_bytes) {
        too_large = true;
        session->release_body(this);
        return;
    }
    body.append(reinterpret_cast<const char*>(data), len);
    request_body_bytes = body.size();
    session->total_body += len;
}

void ProxyServer::Impl::H2Stream::on_end_stream() {
    if (body_done || closed) return;
    body_done = true;
    if (too_large) {
        submit_error(413, "HTTP/2 request body exceeds http2_max_request_body_bytes");
        return;
    }
    if (!valid_token(method)) {
        submit_error(400, "invalid :method");
        return;
    }
    if (iequals(method, "CONNECT")) {
        submit_error(501, "CONNECT is not supported");
        return;
    }
    if (path.empty() || (!valid_request_target(path) && path != "*")) {
        submit_error(400, "invalid :path");
        return;
    }
    if (path != "*" && path.front() != '/') {
        submit_error(400, ":path must be origin-form");
        return;
    }
    if (authority.empty() && path != "*") {
        submit_error(400, "missing :authority");
        return;
    }
    const std::string cl = header_value(headers, "content-length");
    if (!cl.empty()) {
        std::uint64_t declared = 0;
        if (!parse_u64(cl, declared) || declared != body.size()) {
            submit_error(400, "HTTP/2 content-length does not match body");
            return;
        }
    }

    req = RequestHead{};
    req.method = method;
    req.target = path;
    req.version = "HTTP/1.1";
    req.http10 = false;
    req.host = authority;
    req.headers = headers;
    req.content_length = body.size();
    req.has_content_length =
        !body.empty() || !cl.empty() || iequals(method, "POST") || iequals(method, "PUT") ||
        iequals(method, "PATCH");
    req.chunked = false;
    req.keep_alive = true;

    request_id = sanitize_request_id(header_value(headers, "x-request-id"));
    if (request_id.empty()) request_id = "eg-" + random_hex(8);
    client_ip = client ? client->client_ip : "?";
    started_ms = now_ms();
    start_trace();
    dispatch();
}

void ProxyServer::Impl::H2Stream::dispatch() {
    attempts = 0;
    tried.clear();
    request_bytes_written = false;
    state = State::Init;
    try_attempt();
}

void ProxyServer::Impl::H2Stream::try_attempt() {
    for (;;) {
        if (attempts >= server->cfg.max_attempts) {
            submit_error(502, "all upstream attempts failed");
            return;
        }
        Backend* b = server->select_backend(tried);
        if (!b) {
            submit_error(attempts == 0 ? 503 : 502,
                         attempts == 0 ? "no healthy backend available"
                                       : "all upstream attempts failed");
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
            submit_error(502, why.empty() ? "cannot connect upstream" : why);
            return;
        }
        up = candidate;
        if (up->connecting) {
            state = State::Connecting;
            return;
        }
        on_upstream_ready();
        return;
    }
}

void ProxyServer::Impl::H2Stream::on_upstream_ready() {
    if (closed || !up) return;
    const std::int64_t now = now_ms();
    connect_ms = started_ms ? static_cast<double>(now - started_ms) : 0.0;
    server->metrics.upstream_connect_ms.observe(connect_ms);
    if (backend) backend->record_success(now);
    up->connected = true;
    up->connecting = false;
    up->update_events();

    const std::string head = build_request_head(req, backend->endpoint().authority(),
                                                client_ip, request_id,
                                                server->cfg.preserve_host, false);
    up->out.append(head);
    if (req.has_content_length && !body.empty()) up->out.append(body);
    flush_request();
    if (closed || !up) return;
    state = State::Waiting;
    request_sent_ms = now;
    up->update_events();
}

void ProxyServer::Impl::H2Stream::flush_request() {
    if (!up || closed) return;
    while (!up->out.empty()) {
        const ssize_t n = ::send(up->fd, up->out.data(), up->out.size(), MSG_NOSIGNAL);
        if (n > 0) {
            request_bytes_written = true;
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

void ProxyServer::Impl::H2Stream::on_upstream_writable() { flush_request(); }

void ProxyServer::Impl::H2Stream::on_upstream_readable() {
    if (closed || !up || up->connecting || up_read_paused) return;
    if (pending_bytes() >= server->cfg.client_buffer_high) {
        set_up_paused(true);
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
            if (up_read_paused) return;
            if (pending_bytes() >= server->cfg.client_buffer_high) {
                set_up_paused(true);
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
        on_upstream_failure(std::string("upstream read: ") + std::strerror(errno), false, 502);
        return;
    }
    if (up) up->update_events();
}

void ProxyServer::Impl::H2Stream::set_up_paused(bool paused) {
    if (up_read_paused == paused) return;
    up_read_paused = paused;
    if (paused) server->metrics.upstream_read_pauses.inc();
    else server->metrics.upstream_read_resumes.inc();
    if (up) up->set_read_paused(paused);
}

void ProxyServer::Impl::H2Stream::append_pending(std::string_view data) {
    pending.append(data.data(), data.size());
    if (data_deferred) {
        data_deferred = false;
        nghttp2_session_resume_data(session->ns, id);
    }
    session->schedule_flush();
    if (pending_bytes() >= server->cfg.client_buffer_high) set_up_paused(true);
}

void ProxyServer::Impl::H2Stream::process_upstream_input() {
    for (;;) {
        if (closed || !up) return;
        if (!resp_head_done) {
            HttpError err;
            const bool head = iequals(method, "HEAD");
            const auto st = parser.parse(up->in, head, resp, err);
            if (st == ResponseHeadParser::Status::NeedMore) return;
            if (st == ResponseHeadParser::Status::Error) {
                on_upstream_failure(err.message, false, 502);
                return;
            }
            if (resp.status >= 100 && resp.status < 200) {
                parser.reset();
                continue;
            }
            resp_head_done = true;
            response_started = true;
            state = State::Streaming;
            status = resp.status;
            trace.status = resp.status;
            const std::int64_t now = now_ms();
            ttfb_ms = static_cast<double>(now - started_ms);
            server->metrics.ttfb_ms.observe(ttfb_ms);
            if (backend) backend->metrics.ttfb_ms.observe(ttfb_ms);
            if (backend) backend->record_response(now, resp.status);
            resp_mode = parser.body_mode();
            resp_remaining = resp.content_length;
            trace.chunked_response = resp.chunked;
            if (resp.chunked) server->metrics.chunked_responses.inc();
            submit_response_headers();
            if (resp_mode == BodyMode::None) {
                response_complete = true;
                finish_response();
                return;
            }
            session->schedule_flush();
        }

        if (resp_mode == BodyMode::ContentLength) {
            if (resp_remaining == 0) {
                response_complete = true;
                finish_response();
                return;
            }
            if (up->in.empty()) return;
            const std::size_t take = static_cast<std::size_t>(
                std::min<std::uint64_t>(resp_remaining, up->in.size()));
            const std::string_view view(up->in.view().substr(0, take));
            up->in.consume(take);
            resp_remaining -= take;
            response_bytes += take;
            append_pending(view);
            if (resp_remaining == 0) {
                response_complete = true;
                finish_response();
                return;
            }
        } else if (resp_mode == BodyMode::Chunked) {
            std::string_view data;
            HttpError err;
            const auto st = chunks.next(up->in, data, err);
            if (st == ChunkedDecoder::Status::NeedMore) return;
            if (st == ChunkedDecoder::Status::Error) {
                on_upstream_failure(err.message, false, 502);
                return;
            }
            if (st == ChunkedDecoder::Status::Done) {
                response_complete = true;
                finish_response();
                return;
            }
            response_bytes += data.size();
            append_pending(data);
        } else if (resp_mode == BodyMode::UntilClose) {
            if (!up->in.empty()) {
                const std::string_view view = up->in.view();
                response_bytes += view.size();
                append_pending(view);
                up->in.clear();
            }
            if (pending_bytes() >= server->cfg.client_buffer_high) {
                set_up_paused(true);
                return;
            }
            return;
        } else {
            response_complete = true;
            finish_response();
            return;
        }

        if (pending_bytes() >= server->cfg.client_buffer_high) {
            set_up_paused(true);
            return;
        }
    }
}

void ProxyServer::Impl::H2Stream::submit_response_headers() {
    H2NvStore nv;
    nv.add(":status", std::to_string(resp.status));
    for (const Header& h : resp.headers) {
        if (h2_forbidden_header(h.name)) continue;
        if (h.name == "content-length" && resp.chunked) continue;
        nv.add(h.name, h.value);
    }
    nv.add("via", "1.1 edgegate");
    if (backend) nv.add("x-edgegate-upstream", backend->name());
    nv.add("x-edgegate-request-id", request_id);
    nv.build();

    response_headers_sent = true;
    session->release_body(this);
    if (resp_mode == BodyMode::None) {
        response_complete = true;
        nghttp2_submit_response2(session->ns, id, nv.nv.data(), nv.nv.size(), nullptr);
    } else {
        provider.source.ptr = this;
        provider.read_callback = &H2Stream::data_read;
        provider_attached = true;
        nghttp2_submit_response2(session->ns, id, nv.nv.data(), nv.nv.size(), &provider);
    }
}

nghttp2_ssize ProxyServer::Impl::H2Stream::data_read(nghttp2_session*, int32_t,
                                                     std::uint8_t* buf, std::size_t length,
                                                     std::uint32_t* flags,
                                                     nghttp2_data_source* source, void*) {
    auto* self = static_cast<H2Stream*>(source->ptr);
    if (!self) {
        *flags |= NGHTTP2_DATA_FLAG_EOF;
        return 0;
    }
    return self->read_data(buf, length, flags);
}

nghttp2_ssize ProxyServer::Impl::H2Stream::read_data(std::uint8_t* buf, std::size_t length,
                                                     std::uint32_t* flags) {
    const std::size_t available = pending_bytes();
    if (available == 0) {
        if (response_complete || closed) {
            *flags |= NGHTTP2_DATA_FLAG_EOF;
            return 0;
        }
        data_deferred = true;
        return NGHTTP2_ERR_DEFERRED;
    }
    const std::size_t take = std::min(available, length);
    std::memcpy(buf, pending.data() + pending_off, take);
    pending_off += take;
    if (pending_off > 65536 && pending_off * 2 > pending.size()) {
        pending.erase(0, pending_off);
        pending_off = 0;
    }
    if (response_complete && pending_bytes() == 0) {
        *flags |= NGHTTP2_DATA_FLAG_EOF;
    }
    if (up_read_paused && pending_bytes() < server->cfg.client_buffer_low) {
        set_up_paused(false);
    }
    return static_cast<nghttp2_ssize>(take);
}

void ProxyServer::Impl::H2Stream::on_upstream_eof() {
    if (closed) return;
    if (resp_head_done && resp_mode == BodyMode::UntilClose) {
        response_complete = true;
        finish_response();
        return;
    }
    if (!response_started) {
        on_upstream_failure("upstream closed before response", false, 502);
        return;
    }
    if (!response_complete) {
        submit_rst(NGHTTP2_INTERNAL_ERROR);
        commit_trace("upstream truncated response");
    }
}

void ProxyServer::Impl::H2Stream::finish_response() {
    if (closed) return;
    if (up) {
        const bool reusable = up->connected && resp.keep_alive &&
                              resp_mode != BodyMode::UntilClose && up->in.empty();
        if (reusable) server->release_upstream(up, true);
        else server->destroy_upstream(up, false);
        up = nullptr;
    }
    if (data_deferred) {
        data_deferred = false;
        nghttp2_session_resume_data(session->ns, id);
    }
    state = State::Done;
    commit_trace();
    session->schedule_flush();
}

void ProxyServer::Impl::H2Stream::on_upstream_failure(std::string reason, bool connect_failure,
                                                      int status_hint) {
    if (closed) return;
    const bool was_connecting = up && up->connecting;
    const bool timeout = reason.find("timeout") != std::string::npos;
    abort_upstream();
    if (backend) {
        backend->metrics.errors.inc();
        backend->record_failure(now_ms(), connect_failure || was_connecting);
    }
    if (timeout) server->metrics.timeouts.inc();
    else if (connect_failure || was_connecting) server->metrics.connect_errors.inc();
    else server->metrics.read_errors.inc();

    if (response_headers_sent) {
        submit_rst(NGHTTP2_INTERNAL_ERROR);
        commit_trace(reason);
        return;
    }
    if (can_retry()) {
        server->metrics.retries.inc();
        if (backend) backend->metrics.retries.inc();
        trace.retried = true;
        parser.reset();
        chunks.reset();
        resp = ResponseHead{};
        resp_head_done = false;
        response_started = false;
        response_complete = false;
        resp_mode = BodyMode::None;
        resp_remaining = 0;
        pending.clear();
        pending_off = 0;
        data_deferred = false;
        provider_attached = false;
        state = State::Init;
        try_attempt();
        return;
    }
    submit_error(timeout ? 504 : (status_hint ? status_hint : 502), reason);
}

bool ProxyServer::Impl::H2Stream::can_retry() const {
    RetryInput in;
    in.method = method;
    in.attempts_done = attempts;
    in.max_attempts = server->cfg.max_attempts;
    in.idempotent_only = server->cfg.retry_idempotent_only;
    in.request_bytes_written = request_bytes_written;
    in.body_forwarded = request_bytes_written && !body.empty();
    in.body_complete = true;
    in.body_replayable = true;
    in.response_started = response_started;
    return ::eg::can_retry(in);
}

void ProxyServer::Impl::H2Stream::submit_error(int error_status, std::string message) {
    if (closed || committed) return;
    if (response_headers_sent) {
        submit_rst(NGHTTP2_INTERNAL_ERROR);
        commit_trace(message);
        return;
    }
    status = error_status;
    trace.status = error_status;
    trace.error = message;
    std::string text = std::to_string(error_status) + " " + reason_phrase(error_status);
    if (!message.empty()) text += "\n" + message;
    text += "\n";
    pending = std::move(text);
    pending_off = 0;
    response_complete = true;
    response_headers_sent = true;

    H2NvStore nv;
    nv.add(":status", std::to_string(error_status));
    nv.add("content-type", "text/plain; charset=utf-8");
    nv.add("content-length", std::to_string(pending.size()));
    nv.add("via", "1.1 edgegate");
    nv.add("x-edgegate-request-id", request_id);
    nv.build();

    provider.source.ptr = this;
    provider.read_callback = &H2Stream::data_read;
    provider_attached = true;
    nghttp2_submit_response2(session->ns, id, nv.nv.data(), nv.nv.size(), &provider);

    abort_upstream();
    commit_trace(message);
    session->schedule_flush();
}

void ProxyServer::Impl::H2Stream::submit_rst(std::uint32_t code) {
    if (closed) return;
    if (session && session->ns) {
        nghttp2_submit_rst_stream(session->ns, NGHTTP2_FLAG_NONE, id, code);
        session->schedule_flush();
    }
    closed = true;
    abort_upstream();
}

void ProxyServer::Impl::H2Stream::abort_upstream() {
    if (up) {
        up->owner = nullptr;
        server->destroy_upstream(up, true);
        up = nullptr;
    }
}

void ProxyServer::Impl::H2Stream::commit_trace(std::string_view error) {
    if (committed) return;
    committed = true;
    trace.duration_ms = static_cast<double>(now_ms() - started_ms);
    trace.ttfb_ms = ttfb_ms;
    trace.connect_ms = connect_ms;
    trace.bytes_in = request_body_bytes;
    trace.bytes_out = response_bytes;
    trace.attempts = attempts;
    trace.backend = backend ? backend->name() : "";
    trace.status = status;
    if (!error.empty()) trace.error = std::string(error);
    server->metrics.active_requests.dec();
    if (status > 0) server->metrics.responses_total.inc();
    server->commit_trace(trace);
}

void ProxyServer::Impl::H2Stream::on_tick(std::int64_t now) {
    if (closed || state == State::Done) return;
    if (state == State::Waiting && request_sent_ms > 0 &&
        now - request_sent_ms > server->cfg.upstream_response_timeout_ms) {
        on_upstream_failure("upstream response timeout", false, 504);
        return;
    }
    if (!response_started && started_ms > 0 &&
        now - started_ms > server->cfg.request_timeout_ms) {
        on_upstream_failure("request timeout", false, 504);
    }
}

void ProxyServer::Impl::H2Stream::on_close() {
    if (closed) return;
    closed = true;
    abort_upstream();
    if (session) session->release_body(this);
    commit_trace("stream closed");
}

// ---------------------------------------------------------------------------
// H2Session
// ---------------------------------------------------------------------------

bool ProxyServer::Impl::H2Session::init(std::string& error) {
    nghttp2_session_callbacks* callbacks = nullptr;
    if (nghttp2_session_callbacks_new(&callbacks) != 0 || !callbacks) {
        error = "nghttp2_session_callbacks_new failed";
        return false;
    }
    nghttp2_session_callbacks_set_on_begin_headers_callback(callbacks, &H2Session::on_begin_headers);
    nghttp2_session_callbacks_set_on_header_callback(callbacks, &H2Session::on_header);
    nghttp2_session_callbacks_set_on_data_chunk_recv_callback(callbacks, &H2Session::on_data_chunk);
    nghttp2_session_callbacks_set_on_frame_recv_callback(callbacks, &H2Session::on_frame_recv);
    nghttp2_session_callbacks_set_on_stream_close_callback(callbacks, &H2Session::on_stream_close);

    const int rc = nghttp2_session_server_new2(&ns, callbacks, this, nullptr);
    nghttp2_session_callbacks_del(callbacks);
    if (rc != 0 || !ns) {
        error = "nghttp2_session_server_new2 failed";
        return false;
    }

    nghttp2_settings_entry settings[2] = {
        {NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS,
         static_cast<std::uint32_t>(server->cfg.http2_max_concurrent_streams)},
        {NGHTTP2_SETTINGS_INITIAL_WINDOW_SIZE, 1u << 20}};
    if (nghttp2_submit_settings(ns, NGHTTP2_FLAG_NONE, settings, 2) != 0) {
        error = "nghttp2_submit_settings failed";
        shutdown();
        return false;
    }
    last_activity_ms = now_ms();
    flush();
    return true;
}

void ProxyServer::Impl::H2Session::shutdown() {
    if (closed) return;
    closed = true;
    for (auto& [id, stream] : streams) {
        if (stream) stream->on_close();
    }
    streams.clear();
    erase_queue.clear();
    total_body = 0;
    out.clear();
    if (ns) {
        nghttp2_session_del(ns);
        ns = nullptr;
    }
}

void ProxyServer::Impl::H2Session::fail_connection() {
    if (closed) return;
    if (client) client->close_requested = true;
    shutdown();
}

std::uint32_t ProxyServer::Impl::H2Session::interest() const {
    std::uint32_t want = EPOLLRDHUP;
    if (!read_paused && !client_eof && !closed) want |= EPOLLIN;
    if (!out.empty() || need_write) want |= EPOLLOUT;
    return want;
}

void ProxyServer::Impl::H2Session::update_events() {
    if (!client || client->fd < 0 || client->closed) return;
    client->set_events(interest());
}

void ProxyServer::Impl::H2Session::schedule_flush() {
    flush_pending = true;
    if (!in_recv) flush();
}

void ProxyServer::Impl::H2Session::flush() {
    if (closed || !ns) return;
    flush_pending = false;
    for (;;) {
        const std::uint8_t* data = nullptr;
        const ssize_t n = nghttp2_session_mem_send(ns, &data);
        if (n < 0) {
            fail_connection();
            return;
        }
        if (n == 0) break;
        out.append(data, static_cast<std::size_t>(n));
        if (out.size() > 4u * 1024 * 1024) break;
    }
    need_write = false;
    while (!out.empty()) {
        std::size_t wrote = 0;
        const IoStatus st = client->client_write(out.data(), out.size(), wrote);
        if (st == IoStatus::Ok && wrote > 0) {
            out.consume(wrote);
            last_activity_ms = now_ms();
        } else if (st == IoStatus::WantWrite) {
            need_write = true;
            break;
        } else if (st == IoStatus::WantRead) {
            break;
        } else {
            fail_connection();
            return;
        }
    }
    reap();
    update_events();
}

void ProxyServer::Impl::H2Session::on_event(std::uint32_t ev) {
    if (closed) return;
    if (ev & EPOLLERR) {
        fail_connection();
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
    if (ev & (EPOLLHUP | EPOLLRDHUP)) on_client_eof();
}

void ProxyServer::Impl::H2Session::on_readable() {
    if (closed || read_paused || client_eof) return;
    char buf[kRecvChunk];
    for (;;) {
        if (closed || read_paused) break;
        std::size_t got = 0;
        const IoStatus st = client->client_read(buf, sizeof(buf), got);
        if (st == IoStatus::Ok && got > 0) {
            last_activity_ms = now_ms();
            in_recv = true;
            const ssize_t rc = nghttp2_session_mem_recv(ns, reinterpret_cast<std::uint8_t*>(buf), got);
            in_recv = false;
            if (rc < 0) {
                fail_connection();
                return;
            }
            flush();
            if (closed) return;
            if (flush_pending) flush();
            continue;
        }
        if (st == IoStatus::WantRead) break;
        if (st == IoStatus::WantWrite) {
            need_write = true;
            break;
        }
        if (st == IoStatus::Eof) {
            on_client_eof();
            return;
        }
        fail_connection();
        return;
    }
    update_events();
}

void ProxyServer::Impl::H2Session::on_writable() {
    if (closed) return;
    need_write = false;
    flush();
}

void ProxyServer::Impl::H2Session::on_client_eof() {
    if (client_eof || closed) return;
    client_eof = true;
    for (auto& [id, stream] : streams) {
        if (stream) stream->on_close();
    }
    if (client) client->close_requested = true;
}

void ProxyServer::Impl::H2Session::on_tick(std::int64_t now) {
    if (closed) return;
    for (auto& [id, stream] : streams) {
        if (stream) stream->on_tick(now);
    }
    reap();
    if (streams.empty() && out.empty() && now - last_activity_ms > server->cfg.client_idle_timeout_ms) {
        if (client) client->close_requested = true;
    }
}

void ProxyServer::Impl::H2Session::release_body(H2Stream* stream) {
    if (!stream) return;
    if (stream->body.size() <= total_body) total_body -= stream->body.size();
    else total_body = 0;
    stream->body.clear();
}

std::size_t ProxyServer::Impl::H2Session::buffered_bytes() const {
    std::size_t total = out.size();
    for (const auto& [id, stream] : streams) {
        if (!stream) continue;
        total += stream->pending_bytes();
        total += stream->body.size();
    }
    return total;
}

void ProxyServer::Impl::H2Session::reap() {
    for (int32_t id : erase_queue) {
        auto it = streams.find(id);
        if (it == streams.end()) continue;
        if (it->second) {
            it->second->abort_upstream();
            if (it->second->body.size() <= total_body) total_body -= it->second->body.size();
            else total_body = 0;
        }
        streams.erase(it);
    }
    erase_queue.clear();
}

int ProxyServer::Impl::H2Session::on_begin_headers(nghttp2_session* ns,
                                                   const nghttp2_frame* frame, void* user_data) {
    auto* self = static_cast<H2Session*>(user_data);
    if (!self || frame->hd.type != NGHTTP2_HEADERS ||
        frame->headers.cat != NGHTTP2_HCAT_REQUEST) {
        return 0;
    }
    if (self->streams.size() >=
        static_cast<std::size_t>(self->server->cfg.http2_max_concurrent_streams)) {
        nghttp2_submit_rst_stream(ns, NGHTTP2_FLAG_NONE, frame->hd.stream_id,
                                  NGHTTP2_REFUSED_STREAM);
        return 0;
    }
    auto stream = std::make_unique<H2Stream>(self->server, self, self->client, frame->hd.stream_id);
    H2Stream* raw = stream.get();
    self->streams.emplace(frame->hd.stream_id, std::move(stream));
    nghttp2_session_set_stream_user_data(ns, frame->hd.stream_id, raw);
    return 0;
}

int ProxyServer::Impl::H2Session::on_header(nghttp2_session*, const nghttp2_frame* frame,
                                            const std::uint8_t* name, std::size_t namelen,
                                            const std::uint8_t* value, std::size_t valuelen,
                                            std::uint8_t, void* user_data) {
    auto* self = static_cast<H2Session*>(user_data);
    if (!self || frame->hd.type != NGHTTP2_HEADERS) return 0;
    auto it = self->streams.find(frame->hd.stream_id);
    if (it == self->streams.end()) return 0;
    H2Stream* stream = it->second.get();
    const std::string_view n(reinterpret_cast<const char*>(name), namelen);
    const std::string v(reinterpret_cast<const char*>(value), valuelen);
    if (!n.empty() && n[0] == ':') {
        if (n == ":method") stream->method = v;
        else if (n == ":path") stream->path = v;
        else if (n == ":authority") stream->authority = v;
        else if (n == ":scheme") stream->scheme = v;
        return 0;
    }
    if (h2_forbidden_header(n)) return 0;
    stream->headers.push_back(Header{std::string(n), v});
    return 0;
}

int ProxyServer::Impl::H2Session::on_data_chunk(nghttp2_session*, std::uint8_t, int32_t stream_id,
                                                const std::uint8_t* data, std::size_t len,
                                                void* user_data) {
    auto* self = static_cast<H2Session*>(user_data);
    if (!self) return 0;
    auto it = self->streams.find(stream_id);
    if (it != self->streams.end()) it->second->on_data(data, len);
    return 0;
}

int ProxyServer::Impl::H2Session::on_frame_recv(nghttp2_session*, const nghttp2_frame* frame,
                                                void* user_data) {
    auto* self = static_cast<H2Session*>(user_data);
    if (!self) return 0;
    if ((frame->hd.type == NGHTTP2_HEADERS || frame->hd.type == NGHTTP2_DATA) &&
        (frame->hd.flags & NGHTTP2_FLAG_END_STREAM)) {
        auto it = self->streams.find(frame->hd.stream_id);
        if (it != self->streams.end()) it->second->on_end_stream();
    }
    return 0;
}

int ProxyServer::Impl::H2Session::on_stream_close(nghttp2_session*, int32_t stream_id,
                                                  std::uint32_t, void* user_data) {
    auto* self = static_cast<H2Session*>(user_data);
    if (!self) return 0;
    auto it = self->streams.find(stream_id);
    if (it != self->streams.end()) it->second->on_close();
    self->erase_queue.push_back(stream_id);
    return 0;
}

#endif  // EDGEGATE_HAVE_NGHTTP2

void ProxyServer::Impl::ClientConn::start_h2() {
#ifdef EDGEGATE_HAVE_NGHTTP2
    if (!server->cfg.http2 || h2_mode) return;
    auto session = std::make_unique<H2Session>(server, this);
    std::string error;
    if (!session->init(error)) {
        log_msg(LogLevel::Warn, "HTTP/2 init failed for ", client_ip, ": ", error);
        return;
    }
    h2 = std::move(session);
    h2_mode = true;
    log_msg(LogLevel::Info, "HTTP/2 connection from ", client_ip, " (ALPN h2)");
#else
    log_msg(LogLevel::Warn, "client negotiated h2 but EdgeGate was built without nghttp2");
#endif
}

ProxyServer::Impl::ClientConn::~ClientConn() = default;
