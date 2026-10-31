#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "edgegate/config.hpp"
#include "edgegate/metrics.hpp"
#include "edgegate/util.hpp"

namespace eg {

class Backend {
public:
    Backend(const BackendConfig& cfg, const Config& global);

    const std::string& name() const { return name_; }
    const Endpoint& endpoint() const { return endpoint_; }
    std::string url() const { return endpoint_.authority(); }
    std::size_t max_connections() const { return max_connections_; }
    std::size_t max_idle() const { return max_idle_; }

    bool healthy() const { return healthy_; }
    bool draining() const { return draining_; }
    void set_draining(bool value) { draining_ = value; }
    bool selectable(std::int64_t now) const {
        return healthy_ && !draining_ && now >= cooldown_until_ms_;
    }

    void record_success(std::int64_t now);
    void record_failure(std::int64_t now, bool connect_failure);
    void record_response(std::int64_t now, int status);

    bool health_check_due(std::int64_t now, std::int64_t interval_ms) const;
    void mark_health_check(std::int64_t now) { last_health_check_ms_ = now; }
    bool health_check_in_progress() const { return health_in_progress_; }
    void set_health_check_in_progress(bool v) { health_in_progress_ = v; }

    int consecutive_failures() const { return failures_; }
    int consecutive_successes() const { return successes_; }

    BackendMetrics metrics;
    BackendSnapshot snapshot() const;

private:
    void set_healthy(bool value);

    std::string name_;
    Endpoint endpoint_;
    std::size_t max_connections_ = 128;
    std::size_t max_idle_ = 32;
    int healthy_threshold_ = 2;
    int unhealthy_threshold_ = 3;
    std::int64_t cooldown_ms_ = 5000;

    bool healthy_ = true;
    bool draining_ = false;
    int failures_ = 0;
    int successes_ = 0;
    std::int64_t cooldown_until_ms_ = 0;
    std::int64_t last_health_check_ms_ = 0;
    bool health_in_progress_ = false;
};

class Balancer {
public:
    explicit Balancer(std::string algorithm) : algorithm_(std::move(algorithm)) {}
    Backend* select(std::vector<Backend*>& backends, const std::vector<Backend*>& exclude,
                    std::int64_t now);

private:
    std::string algorithm_;
    std::size_t round_robin_ = 0;
};

}  // namespace eg
