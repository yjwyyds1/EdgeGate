#include "edgegate/util.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <iomanip>
#include <random>
#include <sstream>
#include <system_error>

namespace eg {
namespace {

char hex_digit(unsigned v) { return static_cast<char>(v < 10 ? '0' + v : 'a' + (v - 10)); }

std::string errno_text(int e) { return std::string(std::strerror(e)); }

}  // namespace

std::int64_t now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

std::int64_t unix_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

std::string iso8601_now() {
    using namespace std::chrono;
    const auto now = system_clock::now();
    const auto ms = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;
    const std::time_t t = system_clock::to_time_t(now);
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buf[64];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm);
    char out[80];
    std::snprintf(out, sizeof(out), "%s.%03dZ", buf, static_cast<int>(ms.count()));
    return out;
}

std::string http_date_now() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buf[64];
    std::strftime(buf, sizeof(buf), "%a, %d %b %Y %H:%M:%S GMT", &tm);
    return buf;
}

std::string trim(std::string_view s) {
    std::size_t b = 0;
    std::size_t e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n')) --e;
    return std::string(s.substr(b, e - b));
}

std::string to_lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        char ca = a[i], cb = b[i];
        if (ca >= 'A' && ca <= 'Z') ca = static_cast<char>(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = static_cast<char>(cb - 'A' + 'a');
        if (ca != cb) return false;
    }
    return true;
}

std::vector<std::string> split(std::string_view s, char sep) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (start <= s.size()) {
        const std::size_t pos = s.find(sep, start);
        if (pos == std::string_view::npos) {
            out.emplace_back(s.substr(start));
            break;
        }
        out.emplace_back(s.substr(start, pos - start));
        start = pos + 1;
    }
    return out;
}

std::string join(const std::vector<std::string>& parts, std::string_view sep) {
    std::string out;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i) out += sep;
        out += parts[i];
    }
    return out;
}

bool parse_i64(std::string_view s, std::int64_t& out) {
    if (s.empty()) return false;
    std::int64_t v = 0;
    auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
    if (ec != std::errc{} || ptr != s.data() + s.size()) return false;
    out = v;
    return true;
}

bool parse_u64(std::string_view s, std::uint64_t& out) {
    if (s.empty()) return false;
    std::uint64_t v = 0;
    auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
    if (ec != std::errc{} || ptr != s.data() + s.size()) return false;
    out = v;
    return true;
}

std::string random_hex(std::size_t bytes) {
    std::vector<unsigned char> buf(bytes);
    bool ok = false;
    const int fd = ::open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        std::size_t off = 0;
        while (off < bytes) {
            const ssize_t n = ::read(fd, buf.data() + off, bytes - off);
            if (n <= 0) break;
            off += static_cast<std::size_t>(n);
        }
        ::close(fd);
        ok = off == bytes;
    }
    if (!ok) {
        std::random_device rd;
        for (std::size_t i = 0; i < bytes; i += 4) {
            const unsigned v = rd();
            for (std::size_t j = 0; j < 4 && i + j < bytes; ++j) {
                buf[i + j] = static_cast<unsigned char>((v >> (8 * j)) & 0xff);
            }
        }
    }
    std::string out;
    out.reserve(bytes * 2);
    for (unsigned char c : buf) {
        out.push_back(hex_digit(c >> 4));
        out.push_back(hex_digit(c & 0x0f));
    }
    return out;
}

std::string json_escape(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out.push_back(static_cast<char>(c));
                }
        }
    }
    return out;
}

std::string Endpoint::authority() const {
    if (host.find(':') != std::string::npos) return "[" + host + "]:" + std::to_string(port);
    return host + ":" + std::to_string(port);
}

bool parse_endpoint(std::string_view text, Endpoint& out, std::string& error) {
    std::string s = trim(text);
    if (s.empty()) {
        error = "empty address";
        return false;
    }
    if (s.rfind("http://", 0) == 0) s.erase(0, 7);
    else if (s.rfind("https://", 0) == 0) {
        error = "TLS upstreams are not supported in v1";
        return false;
    }
    if (s.find('/') != std::string::npos) {
        error = "address must be host:port without a path";
        return false;
    }
    std::string host;
    std::string port_text;
    if (!s.empty() && s.front() == '[') {
        const std::size_t close = s.find(']');
        if (close == std::string::npos || close + 1 >= s.size() || s[close + 1] != ':') {
            error = "invalid IPv6 address, expected [addr]:port";
            return false;
        }
        host = s.substr(1, close - 1);
        port_text = s.substr(close + 2);
    } else {
        const std::size_t colon = s.rfind(':');
        if (colon == std::string::npos) {
            error = "missing :port";
            return false;
        }
        host = s.substr(0, colon);
        port_text = s.substr(colon + 1);
    }
    std::uint64_t port = 0;
    if (!parse_u64(port_text, port) || port == 0 || port > 65535) {
        error = "invalid port '" + port_text + "'";
        return false;
    }
    if (host.empty()) host = "0.0.0.0";
    out.host = host;
    out.port = static_cast<std::uint16_t>(port);
    return true;
}

bool set_nonblocking(int fd) {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0) return false;
    return ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

bool set_cloexec(int fd) {
    const int flags = ::fcntl(fd, F_GETFD, 0);
    if (flags < 0) return false;
    return ::fcntl(fd, F_SETFD, flags | FD_CLOEXEC) == 0;
}

void set_tcp_nodelay(int fd) {
    int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
}

void set_keepalive(int fd) {
    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
}

int create_listener(const Endpoint& ep, int backlog, std::string& error) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    addrinfo* res = nullptr;
    const std::string port = std::to_string(ep.port);
    const char* host = ep.host.empty() ? nullptr : ep.host.c_str();
    const int gai = ::getaddrinfo(host, port.c_str(), &hints, &res);
    if (gai != 0) {
        error = std::string("getaddrinfo: ") + gai_strerror(gai);
        return -1;
    }
    int fd = -1;
    std::string last_error;
    for (addrinfo* ai = res; ai != nullptr; ai = ai->ai_next) {
        fd = ::socket(ai->ai_family, ai->ai_socktype | SOCK_CLOEXEC, ai->ai_protocol);
        if (fd < 0) {
            last_error = errno_text(errno);
            continue;
        }
        int one = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        if (ai->ai_family == AF_INET6) {
            ::setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &one, sizeof(one));
        }
        if (::bind(fd, ai->ai_addr, ai->ai_addrlen) != 0) {
            last_error = "bind: " + errno_text(errno);
            ::close(fd);
            fd = -1;
            continue;
        }
        if (::listen(fd, backlog) != 0) {
            last_error = "listen: " + errno_text(errno);
            ::close(fd);
            fd = -1;
            continue;
        }
        break;
    }
    ::freeaddrinfo(res);
    if (fd < 0) {
        error = last_error.empty() ? "socket creation failed" : last_error;
        return -1;
    }
    set_nonblocking(fd);
    return fd;
}

int connect_nonblocking(const Endpoint& ep, bool& in_progress, std::string& error) {
    in_progress = false;
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    const std::string port = std::to_string(ep.port);
    const int gai = ::getaddrinfo(ep.host.c_str(), port.c_str(), &hints, &res);
    if (gai != 0) {
        error = std::string("getaddrinfo: ") + gai_strerror(gai);
        return -1;
    }
    int fd = -1;
    std::string last_error;
    for (addrinfo* ai = res; ai != nullptr; ai = ai->ai_next) {
        fd = ::socket(ai->ai_family, ai->ai_socktype | SOCK_CLOEXEC | SOCK_NONBLOCK, ai->ai_protocol);
        if (fd < 0) {
            last_error = errno_text(errno);
            continue;
        }
        const int rc = ::connect(fd, ai->ai_addr, ai->ai_addrlen);
        if (rc == 0) {
            in_progress = false;
            break;
        }
        if (errno == EINPROGRESS) {
            in_progress = true;
            break;
        }
        last_error = "connect: " + errno_text(errno);
        ::close(fd);
        fd = -1;
    }
    ::freeaddrinfo(res);
    if (fd < 0) {
        error = last_error.empty() ? "connect failed" : last_error;
        return -1;
    }
    set_tcp_nodelay(fd);
    return fd;
}

std::string peer_ip(int fd) {
    sockaddr_storage ss{};
    socklen_t len = sizeof(ss);
    if (::getpeername(fd, reinterpret_cast<sockaddr*>(&ss), &len) != 0) return "?";
    char buf[INET6_ADDRSTRLEN] = {0};
    if (ss.ss_family == AF_INET) {
        const auto* a = reinterpret_cast<const sockaddr_in*>(&ss);
        ::inet_ntop(AF_INET, &a->sin_addr, buf, sizeof(buf));
    } else if (ss.ss_family == AF_INET6) {
        const auto* a = reinterpret_cast<const sockaddr_in6*>(&ss);
        ::inet_ntop(AF_INET6, &a->sin6_addr, buf, sizeof(buf));
    } else {
        return "?";
    }
    return buf;
}

std::string fd_description(int fd) { return "fd " + std::to_string(fd); }

}  // namespace eg
