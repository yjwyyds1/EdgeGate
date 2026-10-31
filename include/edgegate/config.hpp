#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "edgegate/util.hpp"

namespace eg {

struct BackendConfig {
    std::string name;
    Endpoint endpoint;
    std::string url;
    std::size_t max_connections = 0;  // 0 = inherit global
    std::size_t max_idle = 0;         // 0 = inherit global
};

struct Config {
    Endpoint listen{"0.0.0.0", 8080};
    Endpoint admin{"127.0.0.1", 9900};
    bool admin_enabled = true;

    std::string balancer = "least_conn";  // round_robin | least_conn
    bool preserve_host = true;
    bool retry_idempotent_only = true;
    int max_attempts = 2;  // 总尝试次数 = 1 + 重试次数
    std::size_t retry_body_buffer_bytes = 1u << 20;

    std::int64_t client_header_timeout_ms = 10000;
    std::int64_t client_idle_timeout_ms = 60000;
    std::int64_t client_write_stall_ms = 30000;
    std::int64_t upstream_connect_timeout_ms = 2000;
    std::int64_t upstream_response_timeout_ms = 30000;
    std::int64_t request_timeout_ms = 120000;
    std::int64_t shutdown_grace_ms = 5000;

    std::size_t max_request_header_bytes = 32768;
    std::size_t max_request_body_bytes = 0;  // 0 = 不限制
    std::size_t max_response_header_bytes = 65536;

    std::size_t client_buffer_high = 256u << 10;
    std::size_t client_buffer_low = 64u << 10;
    std::size_t upstream_buffer_high = 256u << 10;
    std::size_t upstream_buffer_low = 64u << 10;
    std::size_t max_total_buffered = 128u << 20;

    std::size_t max_connections_per_backend = 128;
    std::size_t max_idle_connections_per_backend = 32;
    std::int64_t backend_idle_timeout_ms = 60000;

    std::int64_t health_interval_ms = 5000;  // 0 = 关闭主动检查
    std::int64_t health_timeout_ms = 1000;
    std::string health_path = "/healthz";
    int healthy_threshold = 2;
    int unhealthy_threshold = 3;
    std::int64_t passive_failure_cooldown_ms = 5000;

    std::string access_log = "-";  // "-" = stderr
    std::string log_level = "info";

    std::vector<BackendConfig> backends;

    static bool load(const std::string& path, Config& out, std::string& error);
    std::string summary() const;
};

}  // namespace eg
