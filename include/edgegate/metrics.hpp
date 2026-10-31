#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace eg {

class Counter {
public:
    void inc(std::uint64_t n = 1) { v_.fetch_add(n, std::memory_order_relaxed); }
    void add(std::uint64_t n) { inc(n); }
    std::uint64_t value() const { return v_.load(std::memory_order_relaxed); }
    void reset() { v_.store(0, std::memory_order_relaxed); }

private:
    std::atomic<std::uint64_t> v_{0};
};

class Gauge {
public:
    void inc(std::int64_t n = 1) { v_.fetch_add(n, std::memory_order_relaxed); }
    void dec(std::int64_t n = 1) { v_.fetch_sub(n, std::memory_order_relaxed); }
    void add(std::int64_t n) { inc(n); }
    void set(std::int64_t n) { v_.store(n, std::memory_order_relaxed); }
    std::int64_t value() const { return v_.load(std::memory_order_relaxed); }

private:
    std::atomic<std::int64_t> v_{0};
};

class Histogram {
public:
    static constexpr std::size_t kBucketCount = 12;
    static constexpr std::array<double, kBucketCount> kBounds = {
        1, 2, 5, 10, 25, 50, 100, 250, 500, 1000, 2500, 5000};

    void observe(double ms);
    std::uint64_t count() const { return count_.load(std::memory_order_relaxed); }
    double sum_ms() const {
        return static_cast<double>(sum_us_.load(std::memory_order_relaxed)) / 1000.0;
    }
    std::uint64_t bucket(std::size_t i) const {
        return buckets_[i].load(std::memory_order_relaxed);
    }
    double quantile(double q) const;
    void reset();

private:
    std::array<std::atomic<std::uint64_t>, kBucketCount + 1> buckets_{};
    std::atomic<std::uint64_t> count_{0};
    std::atomic<std::uint64_t> sum_us_{0};
};

struct Metrics {
    Counter requests_total;
    Counter responses_total;
    Counter client_connections_total;
    Counter client_disconnects;
    Counter upstream_connections_total;
    Counter requests_rejected;

    Counter parse_errors;
    Counter connect_errors;
    Counter read_errors;
    Counter write_errors;
    Counter timeouts;
    Counter no_backend;
    Counter body_too_large;
    Counter upstream_eof;
    Counter retries;

    Counter bytes_in_total;
    Counter bytes_out_total;
    Counter chunked_requests;
    Counter chunked_responses;
    Counter http10_requests;

    Counter upstream_read_pauses;
    Counter upstream_read_resumes;
    Counter client_read_pauses;
    Counter client_read_resumes;

    Counter health_checks;
    Counter health_failures;

    Gauge active_clients;
    Gauge active_requests;
    Gauge buffered_bytes;
    Gauge healthy_backends;

    Histogram request_duration_ms;
    Histogram ttfb_ms;
    Histogram upstream_connect_ms;
};

struct BackendMetrics {
    Counter requests;
    Counter errors;
    Counter retries;
    Counter connect_failures;
    Counter responses_2xx;
    Counter responses_3xx;
    Counter responses_4xx;
    Counter responses_5xx;
    Counter bytes_in;
    Counter bytes_out;
    Gauge active_connections;
    Gauge idle_connections;
    Histogram latency_ms;
    Histogram ttfb_ms;
};

struct BackendSnapshot {
    std::string name;
    std::string url;
    bool healthy = true;
    bool draining = false;
    std::uint64_t active_connections = 0;
    std::uint64_t idle_connections = 0;
    std::uint64_t requests = 0;
    std::uint64_t errors = 0;
    std::uint64_t retries = 0;
    std::uint64_t connect_failures = 0;
    std::uint64_t responses_2xx = 0;
    std::uint64_t responses_3xx = 0;
    std::uint64_t responses_4xx = 0;
    std::uint64_t responses_5xx = 0;
    double latency_p50 = 0;
    double latency_p95 = 0;
    double latency_p99 = 0;
    double ttfb_p95 = 0;
};

std::string render_prometheus(const Metrics& metrics,
                              const std::vector<BackendSnapshot>& backends,
                              std::int64_t uptime_seconds);

}  // namespace eg
