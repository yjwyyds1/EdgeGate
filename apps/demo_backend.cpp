// EdgeGate 演示后端：真实 HTTP/1.1 文件与 API 服务，用来验证代理的
// keep-alive、chunked、上传、下载、哈希、健康检查与慢客户端背压。
//
// 它不是生产服务器：线程每连接、功能刻意保持直观，便于阅读和手工验证。

#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "edgegate/buffer.hpp"
#include "edgegate/http.hpp"
#include "edgegate/util.hpp"

// OpenSSL 的 EVP 摘要接口。这里只声明用到的符号，避免依赖开发头文件；
// 运行库由 CMake 的 CRYPTO_LIBRARY 链接。
extern "C" {
struct evp_md_ctx_st;
struct evp_md_st;
const evp_md_st* EVP_sha256();
evp_md_ctx_st* EVP_MD_CTX_new();
void EVP_MD_CTX_free(evp_md_ctx_st*);
int EVP_DigestInit_ex(evp_md_ctx_st*, const evp_md_st*, void*);
int EVP_DigestUpdate(evp_md_ctx_st*, const void*, std::size_t);
int EVP_DigestFinal_ex(evp_md_ctx_st*, unsigned char*, unsigned int*);
}

namespace {

using eg::Buffer;
using eg::ChunkedDecoder;
using eg::HttpError;
using eg::RequestHead;
using eg::RequestHeadParser;

struct Options {
    eg::Endpoint listen{"127.0.0.1", 9001};
    std::string name = "app-a";
    std::filesystem::path root = "./demo-root";
    std::filesystem::path uploads = "./demo-uploads";
    int max_connections = 64;
    bool quiet = false;
};

Options g_options;
std::atomic<int> g_active{0};
std::atomic<std::uint64_t> g_served{0};
std::int64_t g_start_ms = 0;

class Sha256 {
public:
    Sha256() {
        ctx_ = EVP_MD_CTX_new();
        if (ctx_) EVP_DigestInit_ex(ctx_, EVP_sha256(), nullptr);
    }
    ~Sha256() {
        if (ctx_) EVP_MD_CTX_free(ctx_);
    }
    void update(std::string_view data) {
        if (ctx_) EVP_DigestUpdate(ctx_, data.data(), data.size());
    }
    std::string hex() {
        if (!ctx_ || finished_) return std::string(64, '0');
        finished_ = true;
        unsigned char md[32];
        unsigned int len = 0;
        EVP_DigestFinal_ex(ctx_, md, &len);
        static const char* kHex = "0123456789abcdef";
        std::string out;
        out.reserve(64);
        for (unsigned i = 0; i < len; ++i) {
            out.push_back(kHex[md[i] >> 4]);
            out.push_back(kHex[md[i] & 0x0f]);
        }
        return out;
    }

private:
    evp_md_ctx_st* ctx_ = nullptr;
    bool finished_ = false;
};

bool send_all(int fd, std::string_view data) {
    std::size_t off = 0;
    while (off < data.size()) {
        const ssize_t n = ::send(fd, data.data() + off, data.size() - off, MSG_NOSIGNAL);
        if (n > 0) {
            off += static_cast<std::size_t>(n);
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else {
            return false;
        }
    }
    return true;
}

std::string query_value(std::string_view query, std::string_view name) {
    for (const std::string& part : eg::split(query, '&')) {
        const std::size_t eq = part.find('=');
        const std::string_view key = eq == std::string::npos ? part : std::string_view(part).substr(0, eq);
        if (key == name) return eq == std::string::npos ? "" : part.substr(eq + 1);
    }
    return {};
}

int query_int(std::string_view query, std::string_view name, int fallback, int lo, int hi) {
    const std::string v = query_value(query, name);
    std::int64_t parsed = 0;
    if (!eg::parse_i64(v, parsed)) return fallback;
    return static_cast<int>(std::max<std::int64_t>(lo, std::min<std::int64_t>(hi, parsed)));
}

std::string url_decode(std::string_view s, bool& ok) {
    std::string out;
    ok = true;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%') {
            if (i + 2 >= s.size()) { ok = false; return {}; }
            auto hex = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            const int hi = hex(s[i + 1]);
            const int lo = hex(s[i + 2]);
            if (hi < 0 || lo < 0) { ok = false; return {}; }
            out.push_back(static_cast<char>((hi << 4) | lo));
            i += 2;
        } else {
            out.push_back(s[i]);
        }
    }
    return out;
}

bool safe_name(std::string_view name) {
    if (name.empty() || name == "." || name == "..") return false;
    if (name.find('/') != std::string_view::npos || name.find('\\') != std::string_view::npos) return false;
    if (name.find('\0') != std::string_view::npos) return false;
    return true;
}

// 演示用的无状态 token：任一实例都能独立校验，代理把请求分发到不同
// 后端时不会因为内存会话而 401。真实系统应使用签名/JWT 或共享会话存储。
std::string demo_token() {
    Sha256 h;
    h.update("demo:edgegate-demo-secret");
    return "tok-demo-" + h.hex();
}

std::string content_type_for(std::string_view name) {    const std::size_t dot = name.rfind('.');
    const std::string ext = dot == std::string_view::npos ? "" : eg::to_lower(name.substr(dot));
    if (ext == ".html" || ext == ".htm") return "text/html; charset=utf-8";
    if (ext == ".txt" || ext == ".log" || ext == ".md") return "text/plain; charset=utf-8";
    if (ext == ".json") return "application/json; charset=utf-8";
    if (ext == ".png") return "image/png";
    if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
    if (ext == ".gif") return "image/gif";
    if (ext == ".css") return "text/css";
    if (ext == ".js") return "application/javascript";
    return "application/octet-stream";
}

void response_head(std::string& out, int status, std::string_view reason, std::string_view type,
                   std::size_t length, bool keep_alive,
                   const std::vector<std::pair<std::string, std::string>>& extra) {
    out += "HTTP/1.1 " + std::to_string(status) + " " + std::string(reason) + "\r\n";
    out += "Date: " + eg::http_date_now() + "\r\n";
    out += "Server: eg-demo\r\n";
    out += "X-Backend: " + g_options.name + "\r\n";
    out += "Content-Type: " + std::string(type) + "\r\n";
    out += "Content-Length: " + std::to_string(length) + "\r\n";
    out += keep_alive ? "Connection: keep-alive\r\n" : "Connection: close\r\n";
    for (const auto& [k, v] : extra) out += k + ": " + v + "\r\n";
    out += "\r\n";
}

bool send_response(int fd, int status, std::string_view type, std::string_view body,
                   bool keep_alive, bool head_only = false,
                   const std::vector<std::pair<std::string, std::string>>& extra = {}) {
    std::string out;
    response_head(out, status, eg::reason_phrase(status), type, body.size(), keep_alive, extra);
    if (!head_only) out += body;
    return send_all(fd, out);
}

bool send_json(int fd, int status, std::string_view body, bool keep_alive, bool head_only = false) {
    return send_response(fd, status, "application/json; charset=utf-8", body, keep_alive, head_only);
}

std::string json_error(std::string_view message) {
    return "{\"error\":\"" + eg::json_escape(message) + "\"}";
}

bool recv_more(int fd, Buffer& in) {
    char buf[65536];
    for (;;) {
        const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n > 0) {
            in.append(buf, static_cast<std::size_t>(n));
            return true;
        }
        if (n == 0) return false;
        if (errno == EINTR) continue;
        return false;
    }
}

// 把请求体流式交给 sink。返回 200 表示成功，否则是应回给客户端的错误码。
int read_body(int fd, Buffer& in, const RequestHead& req,
              const std::function<bool(std::string_view)>& sink, std::size_t max_bytes) {
    std::size_t total = 0;
    if (req.chunked) {
        ChunkedDecoder decoder;
        for (;;) {
            std::string_view data;
            HttpError err;
            const auto st = decoder.next(in, data, err);
            if (st == ChunkedDecoder::Status::NeedMore) {
                if (!recv_more(fd, in)) return 400;
                continue;
            }
            if (st == ChunkedDecoder::Status::Error) return 400;
            if (st == ChunkedDecoder::Status::Done) return 200;
            total += data.size();
            if (max_bytes && total > max_bytes) return 413;
            if (!sink(data)) return 500;
        }
    }
    std::uint64_t remaining = req.content_length;
    while (remaining > 0) {
        if (in.empty()) {
            if (!recv_more(fd, in)) return 400;
            continue;
        }
        const std::size_t take =
            static_cast<std::size_t>(std::min<std::uint64_t>(remaining, in.size()));
        const std::string_view data = in.view().substr(0, take);
        in.consume(take);
        remaining -= take;
        total += take;
        if (max_bytes && total > max_bytes) return 413;
        if (!sink(data)) return 500;
    }
    return 200;
}

std::string read_body_string(int fd, Buffer& in, const RequestHead& req, std::size_t max_bytes,
                             int& error_status) {
    std::string out;
    error_status = read_body(fd, in, req,
                             [&](std::string_view data) {
                                 if (out.size() + data.size() > max_bytes) return false;
                                 out.append(data.data(), data.size());
                                 return true;
                             },
                             max_bytes);
    return out;
}

bool parse_range(std::string_view header, std::uint64_t size, std::uint64_t& start,
                 std::uint64_t& end) {
    if (header.rfind("bytes=", 0) != 0) return false;
    const std::string_view spec = header.substr(6);
    if (spec.find(',') != std::string_view::npos) return false;
    const std::size_t dash = spec.find('-');
    if (dash == std::string_view::npos) return false;
    const std::string_view a = spec.substr(0, dash);
    const std::string_view b = spec.substr(dash + 1);
    if (a.empty()) {
        std::uint64_t suffix = 0;
        if (!eg::parse_u64(b, suffix) || suffix == 0) return false;
        if (suffix > size) suffix = size;
        start = size - suffix;
        end = size ? size - 1 : 0;
        return size > 0;
    }
    if (!eg::parse_u64(a, start)) return false;
    if (b.empty()) {
        end = size ? size - 1 : 0;
    } else if (!eg::parse_u64(b, end)) {
        return false;
    }
    if (start >= size) return false;
    if (end >= size) end = size - 1;
    if (end < start) return false;
    return true;
}

bool serve_file(int fd, const RequestHead& req, const std::filesystem::path& base,
                std::string_view raw_name, std::string_view url_path, bool keep_alive) {
    bool ok = false;
    const std::string name = url_decode(raw_name, ok);
    if (!ok || !safe_name(name)) {
        return send_json(fd, 400, json_error("invalid file name"), keep_alive);
    }
    const std::filesystem::path path = base / name;
    struct stat st{};
    if (::stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
        return send_json(fd, 404, json_error("file not found"), keep_alive);
    }
    const std::uint64_t size = static_cast<std::uint64_t>(st.st_size);
    const std::string etag = "\"" + std::to_string(st.st_size) + "-" +
                             std::to_string(static_cast<long long>(st.st_mtime)) + "\"";
    const std::string inm = eg::header_value(req.headers, "if-none-match");
    if (!inm.empty() && inm == etag) {
        std::string out;
        response_head(out, 304, "Not Modified", content_type_for(name), 0, keep_alive,
                      {{"ETag", etag}});
        return send_all(fd, out);
    }

    std::uint64_t start = 0;
    std::uint64_t end = size ? size - 1 : 0;
    bool partial = false;
    const std::string range = eg::header_value(req.headers, "range");
    if (!range.empty() && size > 0) {
        if (!parse_range(range, size, start, end)) {
            std::string out;
            response_head(out, 416, "Range Not Satisfiable", "text/plain", 0, keep_alive,
                          {{"Content-Range", "bytes */" + std::to_string(size)}});
            return send_all(fd, out);
        }
        partial = true;
    }
    const std::uint64_t length = size == 0 ? 0 : end - start + 1;

    std::string out;
    response_head(out, partial ? 206 : 200, partial ? "Partial Content" : "OK",
                  content_type_for(name), length, keep_alive,
                  {{"ETag", etag},
                   {"Accept-Ranges", "bytes"},
                   {"Content-Range", partial ? ("bytes " + std::to_string(start) + "-" +
                                                std::to_string(end) + "/" + std::to_string(size))
                                             : ""}});
    // 空 Content-Range 头不合法，去掉。
    if (!partial) {
        const std::string bad = "Content-Range: \r\n";
        const std::size_t pos = out.find(bad);
        if (pos != std::string::npos) out.erase(pos, bad.size());
    }
    if (!send_all(fd, out)) return false;
    if (eg::iequals(req.method, "HEAD")) return true;

    const int file = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (file < 0) return false;
    std::vector<char> buf(65536);
    std::uint64_t offset = start;
    std::uint64_t left = length;
    bool good = true;
    while (left > 0) {
        const std::size_t want = static_cast<std::size_t>(std::min<std::uint64_t>(left, buf.size()));
        const ssize_t n = ::pread(file, buf.data(), want, static_cast<off_t>(offset));
        if (n <= 0) { good = false; break; }
        if (!send_all(fd, std::string_view(buf.data(), static_cast<std::size_t>(n)))) {
            good = false;
            break;
        }
        offset += static_cast<std::uint64_t>(n);
        left -= static_cast<std::uint64_t>(n);
    }
    ::close(file);
    (void)url_path;
    return good;
}

bool upload_file(int fd, Buffer& in, const RequestHead& req, std::string_view raw_name,
                 bool keep_alive) {
    bool ok = false;
    const std::string name = url_decode(raw_name, ok);
    if (!ok || !safe_name(name)) return send_json(fd, 400, json_error("invalid file name"), keep_alive);
    std::filesystem::create_directories(g_options.uploads);
    const std::filesystem::path final_path = g_options.uploads / name;
    const std::filesystem::path temp_path =
        g_options.uploads / (".tmp-" + eg::random_hex(8) + "-" + name);

    const int out = ::open(temp_path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (out < 0) return send_json(fd, 500, json_error("cannot create upload"), keep_alive);
    Sha256 hash;
    std::uint64_t bytes = 0;
    bool write_ok = true;
    const int body_status = read_body(
        fd, in, req,
        [&](std::string_view data) {
            hash.update(data);
            bytes += data.size();
            std::size_t off = 0;
            while (off < data.size()) {
                const ssize_t n = ::write(out, data.data() + off, data.size() - off);
                if (n <= 0) {
                    if (errno == EINTR) continue;
                    write_ok = false;
                    return false;
                }
                off += static_cast<std::size_t>(n);
            }
            return true;
        },
        0);
    if (body_status != 200 || !write_ok) {
        ::close(out);
        ::unlink(temp_path.c_str());
        return send_json(fd, body_status == 200 ? 500 : body_status,
                         json_error("upload body failed"), keep_alive);
    }
    if (::fsync(out) != 0) {
        ::close(out);
        ::unlink(temp_path.c_str());
        return send_json(fd, 500, json_error("upload fsync failed"), keep_alive);
    }
    ::close(out);
    if (::rename(temp_path.c_str(), final_path.c_str()) != 0) {
        ::unlink(temp_path.c_str());
        return send_json(fd, 500, json_error("upload rename failed"), keep_alive);
    }
    const std::string body = "{\"instance\":\"" + g_options.name + "\",\"name\":\"" +
                             eg::json_escape(name) + "\",\"bytes\":" + std::to_string(bytes) +
                             ",\"sha256\":\"" + hash.hex() + "\"}";
    return send_json(fd, 200, body, keep_alive);
}

bool bulk_response(int fd, const RequestHead& req, std::string_view query, bool keep_alive) {
    const int mb = query_int(query, "mb", 16, 1, 1024);
    const std::uint64_t total = static_cast<std::uint64_t>(mb) * 1024 * 1024;
    std::string out;
    response_head(out, 200, "OK", "application/octet-stream", total, keep_alive,
                  {{"X-Pattern", "byte(i)=i&0xff"}});
    if (!send_all(fd, out)) return false;
    if (eg::iequals(req.method, "HEAD")) return true;
    std::vector<char> block(65536);
    for (std::size_t i = 0; i < block.size(); ++i) block[i] = static_cast<char>(i & 0xff);
    std::uint64_t sent = 0;
    while (sent < total) {
        const std::size_t want =
            static_cast<std::size_t>(std::min<std::uint64_t>(total - sent, block.size()));
        if (!send_all(fd, std::string_view(block.data(), want))) return false;
        sent += want;
    }
    return true;
}

std::string index_html() {
    return "<!doctype html><html><head><meta charset=\"utf-8\"><title>eg-demo</title>"
           "<style>body{font:14px system-ui;max-width:760px;margin:40px auto;line-height:1.7}"
           "code{background:#eee;padding:2px 5px;border-radius:4px}</style></head><body>"
           "<h1>EdgeGate 演示后端</h1><p>实例：<b>" + g_options.name +
           "</b></p><ul>"
           "<li><code>GET /healthz</code> 健康检查</li>"
           "<li><code>GET /api/info</code> 实例信息</li>"
           "<li><code>GET /api/bulk?mb=8</code> 大响应（背压测试）</li>"
           "<li><code>POST /api/echo</code> 回显请求体大小与 SHA-256</li>"
           "<li><code>POST /api/login</code> 演示登录，body: {\"user\":\"demo\",\"password\":\"demo\"}</li>"
           "<li><code>GET /api/whoami</code> 需要 <code>Authorization: Bearer &lt;token&gt;</code></li>"
           "<li><code>GET /files/&lt;name&gt;</code> 下载 <code>--root</code> 中的文件</li>"
           "<li><code>PUT /uploads/&lt;name&gt;</code> 上传文件，返回 SHA-256</li>"
           "<li><code>GET /api/delay?ms=1000</code> 延迟响应</li>"
           "</ul></body></html>";
}

void route(int fd, Buffer& in, const RequestHead& req, std::string_view query, bool keep_alive,
           int& out_status, bool& close_after) {
    out_status = 200;
    close_after = !keep_alive;
    const std::string& path = req.target;
    const bool head = eg::iequals(req.method, "HEAD");

    if (path == "/" && (req.method == "GET" || head)) {
        send_response(fd, 200, "text/html; charset=utf-8", index_html(), keep_alive, head);
        return;
    }
    if (path == "/healthz") {
        send_json(fd, 200, "{\"status\":\"ok\",\"instance\":\"" + eg::json_escape(g_options.name) + "\"}",
                  keep_alive, head);
        return;
    }
    if (path == "/api/info") {
        const std::string body = "{\"instance\":\"" + eg::json_escape(g_options.name) +
                                 "\",\"pid\":" + std::to_string(::getpid()) +
                                 ",\"uptime_ms\":" + std::to_string(eg::now_ms() - g_start_ms) +
                                 ",\"served\":" + std::to_string(g_served.load()) +
                                 ",\"root\":\"" + eg::json_escape(g_options.root.string()) +
                                 "\",\"uploads\":\"" + eg::json_escape(g_options.uploads.string()) +
                                 "\"}";
        send_json(fd, 200, body, keep_alive, head);
        return;
    }
    if (path == "/api/echo") {
        if (req.method != "POST" && req.method != "PUT") {
            send_json(fd, 405, json_error("use POST or PUT"), keep_alive, head);
            return;
        }
        Sha256 hash;
        std::uint64_t bytes = 0;
        const int st = read_body(
            fd, in, req,
            [&](std::string_view data) {
                hash.update(data);
                bytes += data.size();
                return true;
            },
            0);
        if (st != 200) {
            send_json(fd, st, json_error("body read failed"), keep_alive, head);
            return;
        }
        const std::string body = "{\"instance\":\"" + eg::json_escape(g_options.name) +
                                 "\",\"bytes\":" + std::to_string(bytes) +
                                 ",\"sha256\":\"" + hash.hex() + "\"}";
        send_json(fd, 200, body, keep_alive, head);
        return;
    }
    if (path == "/api/login" && req.method == "POST") {
        int body_status = 0;
        const std::string body = read_body_string(fd, in, req, 1u << 20, body_status);
        if (body_status != 200) {
            send_json(fd, body_status, json_error("invalid body"), keep_alive, head);
            return;
        }
        if (body.find("\"user\"") == std::string::npos ||
            body.find("\"password\"") == std::string::npos ||
            body.find("demo") == std::string::npos) {
            send_json(fd, 401, json_error("use demo/demo"), keep_alive, head);
            return;
        }
        const std::string token = demo_token();
        send_json(fd, 200,
                  "{\"token\":\"" + token + "\",\"user\":\"demo\",\"instance\":\"" +
                      eg::json_escape(g_options.name) + "\"}",
                  keep_alive, head);
        return;
    }
    if (path == "/api/whoami") {
        std::string auth = eg::header_value(req.headers, "authorization");
        std::string user;
        if (auth.rfind("Bearer ", 0) == 0) {
            const std::string token = auth.substr(7);
            if (token == demo_token()) user = "demo";
        }
        if (user.empty()) {
            send_json(fd, 401, json_error("missing or invalid bearer token"), keep_alive, head);
            return;
        }
        send_json(fd, 200,
                  "{\"user\":\"" + eg::json_escape(user) + "\",\"instance\":\"" +
                      eg::json_escape(g_options.name) + "\"}",
                  keep_alive, head);
        return;
    }
    if (path == "/api/bulk" || path.rfind("/api/bulk?", 0) == 0) {
        bulk_response(fd, req, query, keep_alive);
        return;
    }
    if (path == "/api/chunked") {
        const int chunks = query_int(query, "chunks", 4, 1, 1024);
        const int size = query_int(query, "size", 1024, 0, 1 << 20);
        std::string out;
        out += "HTTP/1.1 200 OK\r\n";
        out += "Date: " + eg::http_date_now() + "\r\n";
        out += "Server: eg-demo\r\n";
        out += "X-Backend: " + g_options.name + "\r\n";
        out += "Content-Type: application/octet-stream\r\n";
        out += "Transfer-Encoding: chunked\r\n";
        out += keep_alive ? "Connection: keep-alive\r\n" : "Connection: close\r\n";
        out += "\r\n";
        if (!head) {
            std::string payload(static_cast<std::size_t>(size), 'z');
            for (int i = 0; i < chunks; ++i) {
                char line[32];
                std::snprintf(line, sizeof(line), "%zx\r\n", payload.size());
                out += line;
                out += payload;
                out += "\r\n";
            }
            out += "0\r\n\r\n";
        }
        send_all(fd, out);
        return;
    }
    if (path == "/api/close") {
        // 无 Content-Length、无 chunked：响应体以关闭连接结束。
        std::string out;
        out += "HTTP/1.1 200 OK\r\n";
        out += "Date: " + eg::http_date_now() + "\r\n";
        out += "Server: eg-demo\r\n";
        out += "X-Backend: " + g_options.name + "\r\n";
        out += "Content-Type: text/plain; charset=utf-8\r\n";
        out += "Connection: close\r\n\r\n";
        out += "close-delimited response from " + g_options.name + "\n";
        send_all(fd, out);
        close_after = true;
        return;
    }
    if (path == "/api/no-content") {
        std::string out;
        out += "HTTP/1.1 204 No Content\r\n";
        out += "Date: " + eg::http_date_now() + "\r\n";
        out += "X-Backend: " + g_options.name + "\r\n";
        out += keep_alive ? "Connection: keep-alive\r\n" : "Connection: close\r\n";
        out += "\r\n";
        send_all(fd, out);
        return;
    }
    if (path == "/api/delay") {
        const int ms = query_int(query, "ms", 1000, 0, 60000);
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
        send_json(fd, 200, "{\"slept_ms\":" + std::to_string(ms) + "}", keep_alive, head);
        return;
    }
    if (path == "/api/fail") {
        const int code = query_int(query, "code", 500, 400, 599);
        send_json(fd, code, json_error("simulated failure"), keep_alive, head);
        out_status = code;
        return;
    }
    if (path.rfind("/files/", 0) == 0) {
        if (req.method != "GET" && !head) {
            send_json(fd, 405, json_error("use GET"), keep_alive, head);
            return;
        }
        serve_file(fd, req, g_options.root, std::string_view(path).substr(7), path, keep_alive);
        return;
    }
    if (path.rfind("/uploads/", 0) == 0) {
        const std::string_view name = std::string_view(path).substr(9);
        if (req.method == "PUT" || req.method == "POST") {
            upload_file(fd, in, req, name, keep_alive);
            return;
        }
        if (req.method == "GET" || head) {
            serve_file(fd, req, g_options.uploads, name, path, keep_alive);
            return;
        }
        send_json(fd, 405, json_error("use PUT or GET"), keep_alive, head);
        return;
    }
    out_status = 404;
    send_json(fd, 404, json_error("not found"), keep_alive, head);
}

void handle_connection(int fd) {
    struct timeval tv{};
    tv.tv_sec = 30;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    int keepalive_on = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &keepalive_on, sizeof(keepalive_on));

    Buffer in;
    RequestHeadParser parser(32768);
    const std::string peer = eg::peer_ip(fd);
    for (;;) {
        RequestHead req;
        HttpError err;
        for (;;) {
            const auto st = parser.parse(in, req, err);
            if (st == RequestHeadParser::Status::Complete) break;
            if (st == RequestHeadParser::Status::Error) {
                send_response(fd, err.status, "text/plain; charset=utf-8", err.message + "\n",
                              false);
                ::close(fd);
                --g_active;
                return;
            }
            if (!recv_more(fd, in)) {
                ::close(fd);
                --g_active;
                return;
            }
        }

        const auto started = std::chrono::steady_clock::now();
        std::string path = req.target;
        std::string query;
        const std::size_t q = path.find('?');
        if (q != std::string::npos) {
            query = path.substr(q + 1);
            path.resize(q);
        }
        // 只允许确实会读取请求体的路由保持 keep-alive；否则残留 body 会被
        // 当成下一个请求解析。演示服务器选择关闭连接而不是做隐式 drain。
        const bool body_route = path == "/api/echo" || path == "/api/login" ||
                                path.rfind("/uploads/", 0) == 0 || path.rfind("/files/", 0) == 0;
        const bool has_body = req.chunked || (req.has_content_length && req.content_length > 0);
        const bool keep_alive = req.keep_alive && (!has_body || body_route);
        RequestHead routed = req;
        routed.target = path;
        int status = 200;
        bool close_after = !keep_alive;
        route(fd, in, routed, query, keep_alive, status, close_after);
        ++g_served;
        if (!g_options.quiet) {
            const auto ms = std::chrono::duration_cast<std::chrono::microseconds>(
                                std::chrono::steady_clock::now() - started)
                                .count() /
                            1000.0;
            std::cout << "[" << g_options.name << "] " << peer << " " << req.method << " "
                      << req.target << " -> " << status << " " << ms << "ms\n";
        }
        if (close_after || !keep_alive) break;
    }
    ::close(fd);
    --g_active;
}

void usage(const char* argv0) {
    std::cout << "eg_demo - EdgeGate demo backend\n\n"
              << "Usage: " << argv0 << " [options]\n"
              << "  --listen HOST:PORT   默认 127.0.0.1:9001\n"
              << "  --name NAME          实例名，默认 app-a\n"
              << "  --root DIR           下载根目录，默认 ./demo-root\n"
              << "  --uploads DIR        上传目录，默认 ./demo-uploads\n"
              << "  --max-connections N  并发连接上限，默认 64\n"
              << "  --quiet              不打印访问日志\n";
}

}  // namespace

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto next = [&](const char* name) -> std::string {
            if (i + 1 >= argc) {
                std::cerr << name << " requires a value\n";
                std::exit(2);
            }
            return argv[++i];
        };
        if (arg == "--listen") {
            std::string error;
            if (!eg::parse_endpoint(next("--listen"), g_options.listen, error)) {
                std::cerr << "invalid --listen: " << error << "\n";
                return 2;
            }
        } else if (arg == "--name") g_options.name = next("--name");
        else if (arg == "--root") g_options.root = next("--root");
        else if (arg == "--uploads") g_options.uploads = next("--uploads");
        else if (arg == "--max-connections") g_options.max_connections = std::stoi(next("--max-connections"));
        else if (arg == "--quiet") g_options.quiet = true;
        else if (arg == "--help" || arg == "-h") {
            usage(argv[0]);
            return 0;
        } else {
            std::cerr << "unknown option: " << arg << "\n";
            usage(argv[0]);
            return 2;
        }
    }

    std::error_code ec;
    std::filesystem::create_directories(g_options.root, ec);
    std::filesystem::create_directories(g_options.uploads, ec);
    ::signal(SIGPIPE, SIG_IGN);
    g_start_ms = eg::now_ms();

    std::string error;
    const int listen_fd = eg::create_listener(g_options.listen, 128, error);
    if (listen_fd < 0) {
        std::cerr << "listen failed: " << error << "\n";
        return 1;
    }
    std::cout << "eg_demo '" << g_options.name << "' listening on " << g_options.listen.authority()
              << ", root=" << g_options.root << ", uploads=" << g_options.uploads << "\n";

    for (;;) {
        const int fd = ::accept4(listen_fd, nullptr, nullptr, SOCK_CLOEXEC);
        if (fd < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                pollfd p{listen_fd, POLLIN, 0};
                ::poll(&p, 1, -1);
                continue;
            }
            std::cerr << "accept failed: " << std::strerror(errno) << "\n";
            continue;
        }
        if (g_active.load() >= g_options.max_connections) {
            const std::string body = "too many connections\n";
            std::string head;
            response_head(head, 503, "Service Unavailable", "text/plain", body.size(), false, {});
            send_all(fd, head + body);
            ::close(fd);
            continue;
        }
        ++g_active;
        std::thread([fd] { handle_connection(fd); }).detach();
    }
}
