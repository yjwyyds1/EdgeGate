#include "edgegate/backend.hpp"

#include <algorithm>
#include <limits>

#include "edgegate/log.hpp"

namespace eg {

Backend::Backend(const BackendConfig& cfg, const Config& global)
    : name_(cfg.name),
      endpoint_(cfg.endpoint),
      max_connections_(cfg.max_connections ? cfg.max_connections
                                           : global.max_connections_per_backend),
      max_idle_(cfg.max_idle ? cfg.max_idle : global.max_idle_connections_per_backend),
      healthy_threshold_(global.healthy_threshold),
      unhealthy_threshold_(global.unhealthy_threshold),
      cooldown_ms_(global.passive_failure_cooldown_ms) {}

void Backend::set_healthy(bool value) {
    if (healthy_ == value) return;
    healthy_ = value;
    log_msg(value ? LogLevel::Info : LogLevel::Warn, "backend ", name_, " is now ",
            value ? "HEALTHY" : "UNHEALTHY");
}

void Backend::record_success(std::int64_t now) {
    (void)now;
    failures_ = 0;
    successes_ = std::min(successes_ + 1, healthy_threshold_);
    if (!healthy_ && successes_ >= healthy_threshold_) {
        set_healthy(true);
        successes_ = 0;
    }
    if (healthy_) {
        successes_ = 0;
        cooldown_until_ms_ = 0;
    }
}

void Backend::record_failure(std::int64_t now, bool connect_failure) {
    successes_ = 0;
    ++failures_;
    if (connect_failure) {
        cooldown_until_ms_ = std::max(cooldown_until_ms_, now + cooldown_ms_);
        metrics.connect_failures.inc();
    }
    if (failures_ >= unhealthy_threshold_) {
        set_healthy(false);
    }
}

void Backend::record_response(std::int64_t now, int status) {
    if (status >= 500) {
        successes_ = 0;
        ++failures_;
        if (failures_ >= unhealthy_threshold_) set_healthy(false);
    } else {
        record_success(now);
    }
    if (status >= 200 && status < 300) metrics.responses_2xx.inc();
    else if (status >= 300 && status < 400) metrics.responses_3xx.inc();
    else if (status >= 400 && status < 500) metrics.responses_4xx.inc();
    else if (status >= 500 && status < 600) metrics.responses_5xx.inc();
}

bool Backend::health_check_due(std::int64_t now, std::int64_t interval_ms) const {
    if (interval_ms <= 0 || health_in_progress_) return false;
    return last_health_check_ms_ == 0 || now - last_health_check_ms_ >= interval_ms;
}

BackendSnapshot Backend::snapshot() const {
    BackendSnapshot s;
    s.name = name_;
    s.url = endpoint_.authority();
    s.healthy = healthy_;
    s.draining = draining_;
    s.active_connections = static_cast<std::uint64_t>(metrics.active_connections.value());
    s.idle_connections = static_cast<std::uint64_t>(metrics.idle_connections.value());
    s.requests = metrics.requests.value();
    s.errors = metrics.errors.value();
    s.retries = metrics.retries.value();
    s.connect_failures = metrics.connect_failures.value();
    s.responses_2xx = metrics.responses_2xx.value();
    s.responses_3xx = metrics.responses_3xx.value();
    s.responses_4xx = metrics.responses_4xx.value();
    s.responses_5xx = metrics.responses_5xx.value();
    s.latency_p50 = metrics.latency_ms.quantile(0.50);
    s.latency_p95 = metrics.latency_ms.quantile(0.95);
    s.latency_p99 = metrics.latency_ms.quantile(0.99);
    s.ttfb_p95 = metrics.ttfb_ms.quantile(0.95);
    return s;
}

Backend* Balancer::select(std::vector<Backend*>& backends, const std::vector<Backend*>& exclude,
                          std::int64_t now) {
    auto excluded = [&](const Backend* b) {
        return std::find(exclude.begin(), exclude.end(), b) != exclude.end();
    };
    if (backends.empty()) return nullptr;

    if (algorithm_ == "round_robin") {
        for (std::size_t i = 0; i < backends.size(); ++i) {
            const std::size_t idx = (round_robin_ + i) % backends.size();
            Backend* b = backends[idx];
            if (b->selectable(now) && !excluded(b)) {
                round_robin_ = (idx + 1) % backends.size();
                return b;
            }
        }
        return nullptr;
    }

    // least_conn：活跃连接最少；同数时按 round-robin 顺序打破平局，避免饥饿。
    Backend* best = nullptr;
    std::int64_t best_load = std::numeric_limits<std::int64_t>::max();
    for (std::size_t i = 0; i < backends.size(); ++i) {
        const std::size_t idx = (round_robin_ + i) % backends.size();
        Backend* b = backends[idx];
        if (!b->selectable(now) || excluded(b)) continue;
        const std::int64_t load = b->metrics.active_connections.value();
        if (load < best_load) {
            best = b;
            best_load = load;
        }
    }
    if (best) {
        round_robin_ = (round_robin_ + 1) % backends.size();
    }
    return best;
}

}  // namespace eg
