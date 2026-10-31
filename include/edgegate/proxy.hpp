#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "edgegate/config.hpp"
#include "edgegate/metrics.hpp"

namespace eg {

struct RequestTrace {
    std::string id;
    std::string method;
    std::string target;
    std::string client_ip;
    std::string backend;
    std::string error;
    int status = 0;
    int attempts = 0;
    std::int64_t start_unix_ms = 0;
    double duration_ms = 0;
    double connect_ms = 0;
    double ttfb_ms = 0;
    std::uint64_t bytes_in = 0;
    std::uint64_t bytes_out = 0;
    bool keep_alive = false;
    bool chunked_request = false;
    bool chunked_response = false;
    bool retried = false;
};

class ProxyServer {
public:
    explicit ProxyServer(Config config);
    ~ProxyServer();

    ProxyServer(const ProxyServer&) = delete;
    ProxyServer& operator=(const ProxyServer&) = delete;

    bool init(std::string& error);
    int run();
    void request_stop();

    const Config& config() const;
    const Metrics& metrics() const;
    std::int64_t uptime_seconds() const;
    std::vector<BackendSnapshot> backend_snapshots() const;

    std::string prometheus() const;
    std::string status_json() const;
    std::string recent_requests_json(std::size_t limit) const;
    std::string trace_json(std::string_view id) const;

    bool set_backend_draining(std::string_view name, bool draining, std::string& error);

    struct Impl;  // 定义在 proxy.cpp，公开仅为让内部连接类型可定义

private:
    std::unique_ptr<Impl> impl_;
};

}  // namespace eg
