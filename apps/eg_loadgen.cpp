// 轻量压测客户端：固定连接数 + HTTP/1.1 keep-alive，测量吞吐和延迟分位数。
// 直接连后端或连 EdgeGate 使用同一份负载，便于做对照实验。

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "edgegate/buffer.hpp"
#include "edgegate/http.hpp"
#include "edgegate/util.hpp"

namespace {

struct Options {
    std::string host = "127.0.0.1";
    std::uint16_t port = 8080;
    std::string host_header;
    std::string path = "/api/info";
    std::string method = "GET";
    int connections = 8;
    std::int64_t requests = 0;  // per connection
    int duration_s = 0;
    std::size_t body_size = 0;
    int timeout_ms = 5000;
    int warmup = 0;
};

struct ThreadResult {
    std::uint64_t requests = 0;
    std::uint64_t errors = 0;
    std::uint64_t non_2xx = 0;
    std::uint64_t bytes = 0;
    std::vector<double> latency_ms;
};

int tcp_connect(const std::string& host, std::uint16_t port, int timeout_ms) {
    eg::Endpoint ep{host, port};
    bool in_progress = false;
    std::string error;
    const int fd = eg::connect_nonblocking(ep, in_progress, error);
    if (fd < 0) return -1;
    if (in_progress) {
        pollfd p{fd, POLLOUT, 0};
        const int rc = ::poll(&p, 1, timeout_ms);
        if (rc <= 0) {
            ::close(fd);
            return -1;
        }
        int err = 0;
        socklen_t len = sizeof(err);
        if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) != 0 || err != 0) {
            ::close(fd);
            return -1;
        }
    }
    timeval tv{};
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    // connect_nonblocking 返回的是非阻塞 socket；压测线程用阻塞读写 + SO_*TIMEO
    // 更简单，也避免把“暂时无数据”误判成连接失败。
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags >= 0) ::fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
    return fd;
}

bool send_all(int fd, std::string_view data) {
    std::size_t off = 0;
    while (off < data.size()) {
        const ssize_t n = ::send(fd, data.data() + off, data.size() - off, MSG_NOSIGNAL);
        if (n > 0) off += static_cast<std::size_t>(n);
        else if (n < 0 && errno == EINTR) continue;
        else return false;
    }
    return true;
}

bool recv_more(int fd, eg::Buffer& in) {
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

// 返回 false 表示连接不可继续复用；*ok=false 表示请求失败。
bool read_response(int fd, eg::Buffer& in, bool& ok, std::uint64_t& body_bytes) {
    ok = false;
    body_bytes = 0;
    eg::ResponseHeadParser parser(65536);
    eg::ResponseHead head;
    eg::HttpError err;
    for (;;) {
        const auto st = parser.parse(in, false, head, err);
        if (st == eg::ResponseHeadParser::Status::Complete) break;
        if (st == eg::ResponseHeadParser::Status::Error) return false;
        if (!recv_more(fd, in)) return false;
    }
    if (head.status >= 100 && head.status < 200) {
        // 跳过 1xx，继续读最终响应。
        for (;;) {
            const auto st = parser.parse(in, false, head, err);
            if (st == eg::ResponseHeadParser::Status::Complete) break;
            if (st == eg::ResponseHeadParser::Status::Error) return false;
            if (!recv_more(fd, in)) return false;
        }
    }

    ok = head.status >= 200 && head.status < 300;
    const eg::BodyMode mode = parser.body_mode();
    if (mode == eg::BodyMode::UntilClose) {
        body_bytes += in.size();
        in.clear();
        char buf[65536];
        for (;;) {
            const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
            if (n > 0) body_bytes += static_cast<std::uint64_t>(n);
            else if (n == 0) break;
            else if (errno == EINTR) continue;
            else return false;
        }
        return false;
    }
    if (mode == eg::BodyMode::Chunked) {
        eg::ChunkedDecoder decoder;
        for (;;) {
            std::string_view data;
            const auto st = decoder.next(in, data, err);
            if (st == eg::ChunkedDecoder::Status::NeedMore) {
                if (!recv_more(fd, in)) return false;
                continue;
            }
            if (st == eg::ChunkedDecoder::Status::Error) return false;
            if (st == eg::ChunkedDecoder::Status::Done) break;
            body_bytes += data.size();
        }
    } else if (mode == eg::BodyMode::ContentLength) {
        std::uint64_t remaining = head.content_length;
        while (remaining > 0) {
            if (in.empty() && !recv_more(fd, in)) return false;
            const std::size_t take =
                static_cast<std::size_t>(std::min<std::uint64_t>(remaining, in.size()));
            in.consume(take);
            remaining -= take;
            body_bytes += take;
        }
    }
    return head.keep_alive;
}

void worker(int id, const Options& opt, std::atomic<bool>& stop, ThreadResult& result) {
    using Clock = std::chrono::steady_clock;
    const std::string body(opt.body_size, 'x');
    std::string request;
    request.reserve(256 + body.size());
    request += opt.method + " " + opt.path + " HTTP/1.1\r\n";
    request += "Host: " + opt.host_header + "\r\n";
    request += "User-Agent: eg_loadgen\r\nAccept: */*\r\nConnection: keep-alive\r\n";
    if (opt.body_size > 0) request += "Content-Length: " + std::to_string(opt.body_size) + "\r\n";
    request += "\r\n";
    request += body;

    int fd = -1;
    eg::Buffer in;
    const auto deadline = Clock::now() + std::chrono::seconds(opt.duration_s);
    std::uint64_t sequence = 0;
    while (!stop.load(std::memory_order_relaxed)) {
        if (opt.duration_s > 0) {
            if (Clock::now() >= deadline) break;
        } else if (sequence >= static_cast<std::uint64_t>(opt.requests)) {
            break;
        }
        ++sequence;
        if (fd < 0) {
            fd = tcp_connect(opt.host, opt.port, opt.timeout_ms);
            if (fd < 0) {
                ++result.errors;
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                continue;
            }
            in.clear();
        }
        const auto start = Clock::now();
        const bool sent = send_all(fd, request);
        bool ok = false;
        std::uint64_t bytes = 0;
        const bool reusable = sent && read_response(fd, in, ok, bytes);
        const auto end = Clock::now();
        if (!sent) {
            ++result.errors;
            ::close(fd);
            fd = -1;
            continue;
        }
        if (!ok) ++result.non_2xx;
        ++result.requests;
        result.bytes += bytes;
        if (sequence > static_cast<std::uint64_t>(opt.warmup)) {
            result.latency_ms.push_back(
                std::chrono::duration<double, std::milli>(end - start).count());
        }
        if (!reusable) {
            ::close(fd);
            fd = -1;
        }
    }
    if (fd >= 0) ::close(fd);
    (void)id;
}

double percentile(const std::vector<double>& sorted, double q) {
    if (sorted.empty()) return 0;
    const std::size_t idx = static_cast<std::size_t>(
        std::ceil(q * static_cast<double>(sorted.size()))) - 1;
    return sorted[std::min(idx, sorted.size() - 1)];
}

void usage(const char* argv0) {
    std::cout << "eg_loadgen - HTTP/1.1 keep-alive load generator\n\n"
              << "Usage: " << argv0 << " --url http://host:port [options]\n"
              << "  --url URL            目标，如 http://127.0.0.1:8080\n"
              << "  --path PATH          请求路径，默认 /api/info\n"
              << "  --connections N      并发连接数，默认 8\n"
              << "  --requests N         每连接请求数，默认 1000\n"
              << "  --duration S         按时间压测，覆盖 --requests\n"
              << "  --method M           默认 GET\n"
              << "  --body-size N        请求体字节数（>0 时建议 --method POST）\n"
              << "  --timeout-ms N       默认 5000\n"
              << "  --warmup N           前 N 个请求不计入延迟\n";
}

}  // namespace

int main(int argc, char** argv) {
    Options opt;
    std::string url;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto next = [&](const char* name) -> std::string {
            if (i + 1 >= argc) {
                std::cerr << name << " requires a value\n";
                std::exit(2);
            }
            return argv[++i];
        };
        if (arg == "--url") url = next("--url");
        else if (arg == "--path") opt.path = next("--path");
        else if (arg == "--connections") opt.connections = std::stoi(next("--connections"));
        else if (arg == "--requests") opt.requests = std::stoll(next("--requests"));
        else if (arg == "--duration") opt.duration_s = std::stoi(next("--duration"));
        else if (arg == "--method") opt.method = next("--method");
        else if (arg == "--body-size") opt.body_size = std::stoull(next("--body-size"));
        else if (arg == "--timeout-ms") opt.timeout_ms = std::stoi(next("--timeout-ms"));
        else if (arg == "--warmup") opt.warmup = std::stoi(next("--warmup"));
        else if (arg == "--help" || arg == "-h") {
            usage(argv[0]);
            return 0;
        } else {
            std::cerr << "unknown option: " << arg << "\n";
            usage(argv[0]);
            return 2;
        }
    }

    if (url.empty()) {
        std::cerr << "--url is required\n";
        usage(argv[0]);
        return 2;
    }
    std::string error;
    eg::Endpoint ep;
    if (!eg::parse_endpoint(url, ep, error)) {
        std::cerr << "invalid --url: " << error << "\n";
        return 2;
    }
    opt.host = ep.host;
    opt.port = ep.port;
    opt.host_header = ep.authority();
    if (opt.connections < 1) opt.connections = 1;
    if (opt.requests <= 0 && opt.duration_s <= 0) opt.requests = 1000;

    std::vector<ThreadResult> results(static_cast<std::size_t>(opt.connections));
    std::vector<std::thread> threads;
    std::atomic<bool> stop{false};
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < opt.connections; ++i) {
        threads.emplace_back(worker, i, std::cref(opt), std::ref(stop), std::ref(results[i]));
    }
    for (auto& t : threads) t.join();
    const auto elapsed = std::chrono::duration<double>(
                             std::chrono::steady_clock::now() - start)
                             .count();

    ThreadResult total;
    for (const ThreadResult& r : results) {
        total.requests += r.requests;
        total.errors += r.errors;
        total.non_2xx += r.non_2xx;
        total.bytes += r.bytes;
        total.latency_ms.insert(total.latency_ms.end(), r.latency_ms.begin(), r.latency_ms.end());
    }
    std::sort(total.latency_ms.begin(), total.latency_ms.end());
    const double qps = elapsed > 0 ? static_cast<double>(total.requests) / elapsed : 0;
    std::cout.setf(std::ios::fixed);
    std::cout.precision(3);
    std::cout << "requests=" << total.requests << " errors=" << total.errors
              << " non_2xx=" << total.non_2xx << " connections=" << opt.connections
              << " elapsed_s=" << elapsed << " qps=" << qps << " bytes=" << total.bytes
              << " p50_ms=" << percentile(total.latency_ms, 0.50)
              << " p90_ms=" << percentile(total.latency_ms, 0.90)
              << " p95_ms=" << percentile(total.latency_ms, 0.95)
              << " p99_ms=" << percentile(total.latency_ms, 0.99)
              << " max_ms=" << (total.latency_ms.empty() ? 0.0 : total.latency_ms.back())
              << "\n";
    return 0;
}
