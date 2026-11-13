// 最小 HTTP/2 h2c（prior knowledge）echo 服务，专供上游 h2 集成测试。

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <iostream>
#include <list>
#include <string>
#include <string_view>

#include <nghttp2/nghttp2.h>

#include "edgegate/util.hpp"

namespace {

struct Stream {
    int32_t id = 0;
    std::string method;
    std::string path;
    std::string body;
    std::string resp;
    bool done = false;
};

struct Session {
    nghttp2_session* ns = nullptr;
    int fd = -1;
    std::list<Stream> streams;
};

Stream* find_stream(Session* s, int32_t id) {
    for (auto& st : s->streams) {
        if (st.id == id) return &st;
    }
    return nullptr;
}

int on_begin_headers(nghttp2_session*, const nghttp2_frame* frame, void* user_data) {
    auto* s = static_cast<Session*>(user_data);
    if (frame->hd.type != NGHTTP2_HEADERS || frame->headers.cat != NGHTTP2_HCAT_REQUEST) return 0;
    s->streams.push_back(Stream{});
    s->streams.back().id = frame->hd.stream_id;
    return 0;
}

int on_header(nghttp2_session*, const nghttp2_frame* frame, const uint8_t* name, size_t namelen,
              const uint8_t* value, size_t valuelen, uint8_t, void* user_data) {
    auto* s = static_cast<Session*>(user_data);
    if (frame->hd.type != NGHTTP2_HEADERS) return 0;
    Stream* st = find_stream(s, frame->hd.stream_id);
    if (!st) return 0;
    std::string_view n(reinterpret_cast<const char*>(name), namelen);
    std::string v(reinterpret_cast<const char*>(value), valuelen);
    if (n == ":method") st->method = std::move(v);
    else if (n == ":path") st->path = std::move(v);
    return 0;
}

int on_data(nghttp2_session*, uint8_t, int32_t stream_id, const uint8_t* data, size_t len,
            void* user_data) {
    auto* s = static_cast<Session*>(user_data);
    Stream* st = find_stream(s, stream_id);
    if (!st) return 0;
    st->body.append(reinterpret_cast<const char*>(data), len);
    return 0;
}

nghttp2_ssize data_read(nghttp2_session*, int32_t, uint8_t* buf, size_t length, uint32_t* flags,
                        nghttp2_data_source* source, void*) {
    auto* body = static_cast<std::string*>(source->ptr);
    if (!body || body->empty()) {
        *flags |= NGHTTP2_DATA_FLAG_EOF;
        return 0;
    }
    const size_t n = std::min(length, body->size());
    std::memcpy(buf, body->data(), n);
    body->erase(0, n);
    if (body->empty()) *flags |= NGHTTP2_DATA_FLAG_EOF;
    return static_cast<nghttp2_ssize>(n);
}

int on_frame_recv(nghttp2_session* session, const nghttp2_frame* frame, void* user_data) {
    auto* s = static_cast<Session*>(user_data);
    if (!(frame->hd.flags & NGHTTP2_FLAG_END_STREAM)) return 0;
    if (frame->hd.type != NGHTTP2_HEADERS && frame->hd.type != NGHTTP2_DATA) return 0;
    Stream* st = find_stream(s, frame->hd.stream_id);
    if (!st || st->done) return 0;
    st->done = true;
    if (st->path == "/healthz") {
        st->resp = "{\"status\":\"ok\",\"h2\":true}";
    } else if (st->path.rfind("/api/echo", 0) == 0) {
        st->resp = "{\"bytes\":" + std::to_string(st->body.size()) + ",\"echo\":true,\"h2\":true}";
    } else {
        st->resp = "{\"path\":\"" + st->path + "\",\"method\":\"" + st->method + "\",\"h2\":true}";
    }

    std::string status = "200";
    std::string ctype = "application/json";
    nghttp2_nv hdrs[2];
    hdrs[0].name = reinterpret_cast<uint8_t*>(const_cast<char*>(":status"));
    hdrs[0].namelen = 7;
    hdrs[0].value = reinterpret_cast<uint8_t*>(status.data());
    hdrs[0].valuelen = status.size();
    hdrs[0].flags = NGHTTP2_NV_FLAG_NONE;
    hdrs[1].name = reinterpret_cast<uint8_t*>(const_cast<char*>("content-type"));
    hdrs[1].namelen = 12;
    hdrs[1].value = reinterpret_cast<uint8_t*>(ctype.data());
    hdrs[1].valuelen = ctype.size();
    hdrs[1].flags = NGHTTP2_NV_FLAG_NONE;

    nghttp2_data_provider2 provider{};
    provider.source.ptr = &st->resp;
    provider.read_callback = data_read;
    nghttp2_submit_response2(session, st->id, hdrs, 2, &provider);
    return 0;
}

bool flush(Session* s) {
    for (;;) {
        const uint8_t* data = nullptr;
        const ssize_t n = nghttp2_session_mem_send(s->ns, &data);
        if (n < 0) return false;
        if (n == 0) break;
        std::size_t off = 0;
        while (off < static_cast<std::size_t>(n)) {
            const ssize_t w =
                ::send(s->fd, data + off, static_cast<std::size_t>(n) - off, MSG_NOSIGNAL);
            if (w > 0) {
                off += static_cast<std::size_t>(w);
                continue;
            }
            if (w < 0 && errno == EINTR) continue;
            return false;
        }
    }
    return true;
}

void serve(int fd) {
    char preface[24];
    std::size_t got = 0;
    while (got < 24) {
        const ssize_t n = ::recv(fd, preface + got, 24 - got, 0);
        if (n <= 0) return;
        got += static_cast<std::size_t>(n);
    }
    if (std::string_view(preface, 24) != "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n") return;

    Session s;
    s.fd = fd;
    nghttp2_session_callbacks* cb = nullptr;
    nghttp2_session_callbacks_new(&cb);
    nghttp2_session_callbacks_set_on_begin_headers_callback(cb, on_begin_headers);
    nghttp2_session_callbacks_set_on_header_callback(cb, on_header);
    nghttp2_session_callbacks_set_on_data_chunk_recv_callback(cb, on_data);
    nghttp2_session_callbacks_set_on_frame_recv_callback(cb, on_frame_recv);
    nghttp2_session_server_new2(&s.ns, cb, &s, nullptr);
    nghttp2_session_callbacks_del(cb);

    nghttp2_settings_entry settings[] = {{NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, 100}};
    nghttp2_submit_settings(s.ns, NGHTTP2_FLAG_NONE, settings, 1);
    if (!flush(&s)) {
        nghttp2_session_del(s.ns);
        return;
    }

    char buf[16384];
    for (;;) {
        const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n == 0) break;
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (nghttp2_session_mem_recv(s.ns, reinterpret_cast<uint8_t*>(buf),
                                     static_cast<std::size_t>(n)) < 0) {
            break;
        }
        if (!flush(&s)) break;
        if (!nghttp2_session_want_read(s.ns) && !nghttp2_session_want_write(s.ns)) break;
    }
    nghttp2_session_del(s.ns);
}

}  // namespace

int main(int argc, char** argv) {
    const char* listen = "127.0.0.1:9401";
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--listen" && i + 1 < argc) listen = argv[++i];
        else if (a == "--help") {
            std::cout << "eg_h2echo --listen 127.0.0.1:9401\n";
            return 0;
        }
    }
    eg::Endpoint ep;
    std::string why;
    if (!eg::parse_endpoint(listen, ep, why)) {
        std::cerr << why << "\n";
        return 1;
    }
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        std::cerr << "socket failed\n";
        return 1;
    }
    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(ep.port));
    if (::inet_pton(AF_INET, ep.host.c_str(), &addr.sin_addr) != 1) {
        std::cerr << "bad host\n";
        ::close(fd);
        return 1;
    }
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
        ::listen(fd, 128) != 0) {
        std::cerr << "bind/listen failed: " << std::strerror(errno) << "\n";
        ::close(fd);
        return 1;
    }
    std::cerr << "eg_h2echo listening on " << ep.authority() << " (h2c)\n";
    for (;;) {
        const int c = ::accept(fd, nullptr, nullptr);
        if (c < 0) {
            if (errno == EINTR) continue;
            break;
        }
        serve(c);
        ::close(c);
    }
    ::close(fd);
    return 0;
}
