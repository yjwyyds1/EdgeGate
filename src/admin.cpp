#include "edgegate/admin.hpp"

#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <sstream>

#include "edgegate/buffer.hpp"
#include "edgegate/http.hpp"
#include "edgegate/log.hpp"
#include "edgegate/proxy.hpp"
#include "edgegate/util.hpp"

namespace eg {
namespace {

constexpr std::size_t kMaxAdminRequest = 16384;

bool eagain(int e) { return e == EAGAIN || e == EWOULDBLOCK; }

std::string status_text(int status) {
    switch (status) {
        case 200: return "OK";
        case 400: return "Bad Request";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 413: return "Payload Too Large";
        case 500: return "Internal Server Error";
        default: return "Status";
    }
}

std::string dashboard_html() {
    return R"HTML(<!doctype html>
<html lang="zh-CN"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>EdgeGate 控制台</title>
<style>
:root{--bg:#0f1115;--panel:#171a21;--line:#272c36;--text:#e7e9ee;--muted:#8b93a7;--ok:#42d392;--warn:#ffb454;--bad:#ff6b6b;--accent:#7aa2ff}
*{box-sizing:border-box}body{margin:0;font:14px/1.5 ui-sans-serif,system-ui,"Segoe UI",Roboto,Arial;background:var(--bg);color:var(--text)}
header{display:flex;align-items:baseline;gap:12px;padding:20px 28px;border-bottom:1px solid var(--line)}
h1{font-size:20px;margin:0;letter-spacing:.3px}h2{font-size:14px;margin:0 0 12px;color:var(--muted);font-weight:600;text-transform:uppercase;letter-spacing:.08em}
main{padding:22px 28px;max-width:1200px;margin:0 auto}
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(150px,1fr));gap:12px;margin-bottom:22px}
.card{background:var(--panel);border:1px solid var(--line);border-radius:10px;padding:14px 16px}
.card .k{color:var(--muted);font-size:12px}.card .v{font-size:24px;font-variant-numeric:tabular-nums;margin-top:4px}
section{margin-bottom:22px}table{width:100%;border-collapse:collapse;background:var(--panel);border:1px solid var(--line);border-radius:10px;overflow:hidden}
th,td{padding:9px 12px;text-align:left;border-bottom:1px solid var(--line);font-variant-numeric:tabular-nums}th{color:var(--muted);font-size:12px;font-weight:600}
tr:last-child td{border-bottom:0}.muted{color:var(--muted)}.ok{color:var(--ok)}.warn{color:var(--warn)}.bad{color:var(--bad)}
a{color:var(--accent);text-decoration:none}.tag{display:inline-block;padding:1px 7px;border-radius:999px;font-size:12px;border:1px solid var(--line)}
code{background:#0b0d12;border:1px solid var(--line);padding:1px 5px;border-radius:4px}
</style></head><body>
<header><h1>EdgeGate</h1><span class="muted">HTTP/1.1 反向代理 · 实时状态</span>
<span class="muted" id="stamp" style="margin-left:auto"></span></header>
<main>
<div class="grid">
 <div class="card"><div class="k">请求总数</div><div class="v" id="req">-</div></div>
 <div class="card"><div class="k">活跃客户端</div><div class="v" id="clients">-</div></div>
 <div class="card"><div class="k">活跃请求</div><div class="v" id="active">-</div></div>
 <div class="card"><div class="k">缓冲占用</div><div class="v" id="buf">-</div></div>
 <div class="card"><div class="k">P95 延迟</div><div class="v" id="p95">-</div></div>
 <div class="card"><div class="k">健康后端</div><div class="v" id="healthy">-</div></div>
</div>
<section><h2>后端</h2><table><thead><tr><th>名称</th><th>地址</th><th>状态</th><th>活跃/空闲连接</th><th>请求</th><th>错误</th><th>P95</th></tr></thead><tbody id="backends"></tbody></table></section>
<section><h2>最近请求</h2><table><thead><tr><th>时间</th><th>方法</th><th>路径</th><th>状态</th><th>后端</th><th>耗时</th><th>尝试</th><th>Request ID</th></tr></thead><tbody id="requests"></tbody></table></section>
<p class="muted">指标接口：<a href="/metrics">/metrics</a> · 状态 JSON：<a href="/api/status">/api/status</a> · 请求追踪：<code>/api/requests/&lt;id&gt;</code></p>
</main>
<script>
const fmt=n=>new Intl.NumberFormat().format(n);
const ms=v=>v>=1000?(v/1000).toFixed(2)+' s':v.toFixed(v<10?2:1)+' ms';
const bytes=n=>{const u=['B','KiB','MiB','GiB'];let i=0,v=n;while(v>=1024&&i<u.length-1){v/=1024;i++}return v.toFixed(i?1:0)+' '+u[i]};
function esc(s){return String(s??'').replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]))}
async function refresh(){
 try{
  const r=await fetch('/api/status',{cache:'no-store'}); const s=await r.json();
  document.getElementById('stamp').textContent='更新于 '+new Date().toLocaleTimeString();
  document.getElementById('req').textContent=fmt(s.metrics.requests_total);
  document.getElementById('clients').textContent=fmt(s.metrics.active_clients);
  document.getElementById('active').textContent=fmt(s.metrics.active_requests);
  document.getElementById('buf').textContent=bytes(s.metrics.buffered_bytes);
  document.getElementById('p95').textContent=ms(s.metrics.latency_ms.p95);
  document.getElementById('healthy').textContent=s.metrics.healthy_backends+' / '+s.backends.length;
  document.getElementById('backends').innerHTML=s.backends.map(b=>`<tr>
    <td><b>${esc(b.name)}</b></td><td class="muted">${esc(b.url)}</td>
    <td class="${b.healthy?'ok':'bad'}">${b.draining?'draining':(b.healthy?'healthy':'unhealthy')}</td>
    <td>${fmt(b.active_connections)} / ${fmt(b.idle_connections)}</td>
    <td>${fmt(b.requests)}</td><td class="${b.errors?'bad':'muted'}">${fmt(b.errors)}</td><td>${ms(b.latency_p95)}</td></tr>`).join('');
  document.getElementById('requests').innerHTML=s.recent_requests.map(t=>`<tr>
    <td class="muted">${new Date(t.started_unix_ms).toLocaleTimeString()}</td>
    <td>${esc(t.method)}</td><td>${esc(t.path)}</td>
    <td class="${t.status>=500||t.status===0?'bad':(t.status>=400?'warn':'ok')}">${t.status||'-'}</td>
    <td>${esc(t.backend)||'-'}</td><td>${ms(t.duration_ms)}</td><td>${t.attempts}</td>
    <td><a href="/api/requests/${encodeURIComponent(t.id)}">${esc(t.id)}</a></td></tr>`).join('')||'<tr><td colspan="8" class="muted">还没有请求</td></tr>';
 }catch(e){document.getElementById('stamp').textContent='管理接口连接失败';}
}
refresh(); setInterval(refresh,2000);
</script></body></html>)HTML";
}

}  // namespace

struct AdminServer::Conn : EventHandler {
    AdminServer* server = nullptr;
    int fd = -1;
    Buffer in;
    Buffer out;
    RequestHeadParser parser;
    bool closed = false;
    bool responded = false;
    std::int64_t created_ms = 0;

    Conn(AdminServer* s, int socket_fd)
        : server(s), fd(socket_fd), parser(kMaxAdminRequest), created_ms(now_ms()) {}
    ~Conn() override = default;

    void update_events() {
        if (fd < 0 || closed) return;
        std::uint32_t want = EPOLLRDHUP;
        if (!responded) want |= EPOLLIN;
        if (!out.empty()) want |= EPOLLOUT;
        server->loop_.mod(fd, want);
    }

    void respond(int status, std::string_view content_type, std::string body) {
        responded = true;
        std::ostringstream os;
        os << "HTTP/1.1 " << status << ' ' << status_text(status) << "\r\n"
           << "Content-Type: " << content_type << "\r\n"
           << "Content-Length: " << body.size() << "\r\n"
           << "Cache-Control: no-store\r\n"
           << "Connection: close\r\n"
           << "\r\n"
           << body;
        out.append(os.str());
        update_events();
    }

    void respond_json(int status, std::string body) {
        respond(status, "application/json; charset=utf-8", std::move(body));
    }

    void route(const RequestHead& req) {
        std::string path = req.target;
        std::string query;
        const std::size_t q = path.find('?');
        if (q != std::string::npos) {
            query = path.substr(q + 1);
            path.resize(q);
        }

        if (req.method != "GET" && req.method != "POST") {
            respond_json(405, "{\"error\":\"method not allowed\"}");
            return;
        }
        if (path == "/" || path == "/index.html") {
            respond(200, "text/html; charset=utf-8", dashboard_html());
            return;
        }
        if (path == "/healthz") {
            respond_json(200, "{\"status\":\"ok\"}");
            return;
        }
        if (path == "/metrics") {
            respond(200, "text/plain; version=0.0.4; charset=utf-8", server->proxy_.prometheus());
            return;
        }
        if (path == "/api/status") {
            respond_json(200, server->proxy_.status_json());
            return;
        }
        if (path == "/api/requests") {
            std::size_t limit = 20;
            const std::string prefix = "limit=";
            if (query.rfind(prefix, 0) == 0) {
                std::uint64_t v = 0;
                if (parse_u64(query.substr(prefix.size()), v)) {
                    limit = static_cast<std::size_t>(std::min<std::uint64_t>(v, 100));
                }
            }
            respond_json(200, server->proxy_.recent_requests_json(limit));
            return;
        }
        const std::string trace_prefix = "/api/requests/";
        if (path.rfind(trace_prefix, 0) == 0) {
            respond_json(200, server->proxy_.trace_json(path.substr(trace_prefix.size())));
            return;
        }
        const std::string backend_prefix = "/api/backends/";
        if (path.rfind(backend_prefix, 0) == 0 && req.method == "POST") {
            const std::string rest = path.substr(backend_prefix.size());
            const std::size_t slash = rest.rfind('/');
            if (slash != std::string::npos) {
                const std::string name = rest.substr(0, slash);
                const std::string action = rest.substr(slash + 1);
                if (action == "drain" || action == "enable") {
                    std::string error;
                    if (!server->proxy_.set_backend_draining(name, action == "drain", error)) {
                        respond_json(404, "{\"error\":\"" + json_escape(error) + "\"}");
                    } else {
                        respond_json(200, "{\"ok\":true,\"backend\":\"" + json_escape(name) +
                                             "\",\"draining\":" +
                                             (action == "drain" ? "true" : "false") + "}");
                    }
                    return;
                }
            }
        }
        respond_json(404, "{\"error\":\"not found\"}");
    }

    void on_event(int event_fd, std::uint32_t ev) override {
        (void)event_fd;
        if (closed) return;
        if (ev & EPOLLERR) {
            close();
            return;
        }
        if ((ev & EPOLLIN) && !responded) {
            char buf[8192];
            for (;;) {
                const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
                if (n > 0) {
                    in.append(buf, static_cast<std::size_t>(n));
                    if (in.size() > kMaxAdminRequest) {
                        respond_json(413, "{\"error\":\"request too large\"}");
                        break;
                    }
                    RequestHead req;
                    HttpError err;
                    const auto st = parser.parse(in, req, err);
                    if (st == RequestHeadParser::Status::NeedMore) continue;
                    if (st == RequestHeadParser::Status::Error) {
                        respond_json(400, "{\"error\":\"" + json_escape(err.message) + "\"}");
                        break;
                    }
                    route(req);
                    break;
                }
                if (n == 0) {
                    close();
                    return;
                }
                if (errno == EINTR) continue;
                if (eagain(errno)) break;
                close();
                return;
            }
        }
        if ((ev & EPOLLOUT)) {
            while (!out.empty()) {
                const ssize_t n = ::send(fd, out.data(), out.size(), MSG_NOSIGNAL);
                if (n > 0) out.consume(static_cast<std::size_t>(n));
                else if (n < 0 && errno == EINTR) continue;
                else if (n < 0 && eagain(errno)) break;
                else {
                    close();
                    return;
                }
            }
            if (out.empty() && responded) {
                close();
                return;
            }
            update_events();
        }
        if (ev & (EPOLLHUP | EPOLLRDHUP)) close();
    }

    void close() {
        if (closed) return;
        closed = true;
        if (fd >= 0) {
            server->loop_.close_later(fd);
            server->zombies_.push_back(fd);
            fd = -1;
        }
    }
};

AdminServer::AdminServer(ProxyServer& proxy, const Config& cfg, EventLoop& loop)
    : proxy_(proxy), cfg_(cfg), loop_(loop) {}

AdminServer::~AdminServer() {
    if (listen_fd_ >= 0) ::close(listen_fd_);
}

bool AdminServer::init(std::string& error) {
    listen_fd_ = create_listener(cfg_.admin, 128, error);
    if (listen_fd_ < 0) {
        error = "admin listener: " + error;
        return false;
    }
    if (!loop_.add(listen_fd_, EPOLLIN, this)) {
        error = "cannot register admin listener";
        return false;
    }
    log_msg(LogLevel::Info, "admin console on http://", cfg_.admin.authority(), "/");
    const std::string host = to_lower(cfg_.admin.host);
    if (host != "127.0.0.1" && host != "::1" && host != "localhost") {
        log_msg(LogLevel::Warn,
                "admin listener is not loopback; it has no authentication, bind it to 127.0.0.1");
    }
    return true;
}

void AdminServer::on_event(int fd, std::uint32_t events) {
    if (fd == listen_fd_) {
        if (events & EPOLLIN) accept_all();
        return;
    }
}

void AdminServer::accept_all() {
    for (;;) {
        const int fd = ::accept4(listen_fd_, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd < 0) {
            if (errno == EINTR) continue;
            if (!eagain(errno)) log_msg(LogLevel::Warn, "admin accept: ", std::strerror(errno));
            break;
        }
        auto conn = std::make_unique<Conn>(this, fd);
        Conn* raw = conn.get();
        conns_.emplace(fd, std::move(conn));
        if (!loop_.add(fd, EPOLLIN | EPOLLRDHUP, raw)) {
            conns_.erase(fd);
            ::close(fd);
            continue;
        }
        raw->update_events();
    }
}

void AdminServer::on_tick(std::int64_t now) {
    reap();
    std::vector<Conn*> idle;
    for (auto& [fd, c] : conns_) {
        if (!c->closed && now - c->created_ms > 30000) idle.push_back(c.get());
    }
    for (Conn* c : idle) c->close();
    reap();
}

void AdminServer::reap() {
    for (int fd : zombies_) conns_.erase(fd);
    zombies_.clear();
}

void AdminServer::close() {
    if (listen_fd_ >= 0) {
        loop_.close_later(listen_fd_);
        listen_fd_ = -1;
    }
    for (auto& [fd, c] : conns_) c->close();
    reap();
}

}  // namespace eg
