#include "edgegate/metrics.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>

namespace eg {

void Histogram::observe(double ms) {
    std::size_t i = 0;
    while (i < kBucketCount && ms > kBounds[i]) ++i;
    buckets_[i].fetch_add(1, std::memory_order_relaxed);
    count_.fetch_add(1, std::memory_order_relaxed);
    sum_us_.fetch_add(static_cast<std::uint64_t>(ms * 1000.0), std::memory_order_relaxed);
}

double Histogram::quantile(double q) const {
    const std::uint64_t n = count();
    if (n == 0) return 0.0;
    const std::uint64_t rank = static_cast<std::uint64_t>(std::ceil(q * static_cast<double>(n)));
    std::uint64_t cumulative = 0;
    for (std::size_t i = 0; i <= kBucketCount; ++i) {
        cumulative += bucket(i);
        if (cumulative >= rank) {
            if (i == kBucketCount) return kBounds.back();
            return kBounds[i];
        }
    }
    return kBounds.back();
}

void Histogram::reset() {
    for (auto& b : buckets_) b.store(0, std::memory_order_relaxed);
    count_.store(0, std::memory_order_relaxed);
    sum_us_.store(0, std::memory_order_relaxed);
}

std::string render_prometheus(const Metrics& m, const std::vector<BackendSnapshot>& backends,
                              std::int64_t uptime_seconds) {
    std::ostringstream os;
    os << "# HELP edgegate_uptime_seconds Process uptime.\n"
       << "# TYPE edgegate_uptime_seconds gauge\n"
       << "edgegate_uptime_seconds " << uptime_seconds << "\n"
       << "# TYPE edgegate_requests_total counter\n"
       << "edgegate_requests_total " << m.requests_total.value() << "\n"
       << "# TYPE edgegate_responses_total counter\n"
       << "edgegate_responses_total " << m.responses_total.value() << "\n"
       << "# TYPE edgegate_client_connections_total counter\n"
       << "edgegate_client_connections_total " << m.client_connections_total.value() << "\n"
       << "# TYPE edgegate_client_disconnects_total counter\n"
       << "edgegate_client_disconnects_total " << m.client_disconnects.value() << "\n"
       << "# TYPE edgegate_upstream_connections_total counter\n"
       << "edgegate_upstream_connections_total " << m.upstream_connections_total.value() << "\n"
       << "# TYPE edgegate_requests_rejected_total counter\n"
       << "edgegate_requests_rejected_total " << m.requests_rejected.value() << "\n"
       << "# TYPE edgegate_active_clients gauge\n"
       << "edgegate_active_clients " << m.active_clients.value() << "\n"
       << "# TYPE edgegate_active_requests gauge\n"
       << "edgegate_active_requests " << m.active_requests.value() << "\n"
       << "# TYPE edgegate_buffered_bytes gauge\n"
       << "edgegate_buffered_bytes " << m.buffered_bytes.value() << "\n"
       << "# TYPE edgegate_healthy_backends gauge\n"
       << "edgegate_healthy_backends " << m.healthy_backends.value() << "\n"
       << "# TYPE edgegate_errors_total counter\n"
       << "edgegate_errors_total{reason=\"parse\"} " << m.parse_errors.value() << "\n"
       << "edgegate_errors_total{reason=\"connect\"} " << m.connect_errors.value() << "\n"
       << "edgegate_errors_total{reason=\"read\"} " << m.read_errors.value() << "\n"
       << "edgegate_errors_total{reason=\"write\"} " << m.write_errors.value() << "\n"
       << "edgegate_errors_total{reason=\"timeout\"} " << m.timeouts.value() << "\n"
       << "edgegate_errors_total{reason=\"no_backend\"} " << m.no_backend.value() << "\n"
       << "edgegate_errors_total{reason=\"body_too_large\"} " << m.body_too_large.value() << "\n"
       << "edgegate_errors_total{reason=\"upstream_eof\"} " << m.upstream_eof.value() << "\n"
       << "# TYPE edgegate_retries_total counter\n"
       << "edgegate_retries_total " << m.retries.value() << "\n"
       << "# TYPE edgegate_bytes_in_total counter\n"
       << "edgegate_bytes_in_total " << m.bytes_in_total.value() << "\n"
       << "# TYPE edgegate_bytes_out_total counter\n"
       << "edgegate_bytes_out_total " << m.bytes_out_total.value() << "\n"
       << "# TYPE edgegate_chunked_requests_total counter\n"
       << "edgegate_chunked_requests_total " << m.chunked_requests.value() << "\n"
       << "# TYPE edgegate_chunked_responses_total counter\n"
       << "edgegate_chunked_responses_total " << m.chunked_responses.value() << "\n"
       << "# TYPE edgegate_http10_requests_total counter\n"
       << "edgegate_http10_requests_total " << m.http10_requests.value() << "\n"
       << "# TYPE edgegate_upstream_read_paused_total counter\n"
       << "edgegate_upstream_read_paused_total " << m.upstream_read_pauses.value() << "\n"
       << "# TYPE edgegate_upstream_read_resumed_total counter\n"
       << "edgegate_upstream_read_resumed_total " << m.upstream_read_resumes.value() << "\n"
       << "# TYPE edgegate_client_read_paused_total counter\n"
       << "edgegate_client_read_paused_total " << m.client_read_pauses.value() << "\n"
       << "# TYPE edgegate_client_read_resumed_total counter\n"
       << "edgegate_client_read_resumed_total " << m.client_read_resumes.value() << "\n"
       << "# TYPE edgegate_health_checks_total counter\n"
       << "edgegate_health_checks_total " << m.health_checks.value() << "\n"
       << "# TYPE edgegate_health_failures_total counter\n"
       << "edgegate_health_failures_total " << m.health_failures.value() << "\n";

    auto histogram = [&](const char* name, const char* help, const Histogram& h) {
        os << "# HELP " << name << " " << help << "\n"
           << "# TYPE " << name << " histogram\n";
        std::uint64_t cumulative = 0;
        for (std::size_t i = 0; i < Histogram::kBucketCount; ++i) {
            cumulative += h.bucket(i);
            os << name << "_bucket{le=\"" << Histogram::kBounds[i] << "\"} " << cumulative << "\n";
        }
        cumulative += h.bucket(Histogram::kBucketCount);
        os << name << "_bucket{le=\"+Inf\"} " << cumulative << "\n"
           << name << "_sum " << h.sum_ms() << "\n"
           << name << "_count " << h.count() << "\n";
    };
    histogram("edgegate_request_duration_ms", "End-to-end request duration in milliseconds.",
              m.request_duration_ms);
    histogram("edgegate_upstream_connect_ms", "Upstream connect latency in milliseconds.",
              m.upstream_connect_ms);
    histogram("edgegate_ttfb_ms", "Time to first response byte in milliseconds.", m.ttfb_ms);

    os << "# TYPE edgegate_backend_healthy gauge\n";
    for (const BackendSnapshot& b : backends) {
        os << "edgegate_backend_healthy{backend=\"" << b.name << "\"} " << (b.healthy ? 1 : 0)
           << "\n";
    }
    os << "# TYPE edgegate_backend_draining gauge\n";
    for (const BackendSnapshot& b : backends) {
        os << "edgegate_backend_draining{backend=\"" << b.name << "\"} " << (b.draining ? 1 : 0)
           << "\n";
    }
    os << "# TYPE edgegate_backend_active_connections gauge\n";
    for (const BackendSnapshot& b : backends) {
        os << "edgegate_backend_active_connections{backend=\"" << b.name << "\"} "
           << b.active_connections << "\n";
    }
    os << "# TYPE edgegate_backend_idle_connections gauge\n";
    for (const BackendSnapshot& b : backends) {
        os << "edgegate_backend_idle_connections{backend=\"" << b.name << "\"} "
           << b.idle_connections << "\n";
    }
    os << "# TYPE edgegate_backend_requests_total counter\n";
    for (const BackendSnapshot& b : backends) {
        os << "edgegate_backend_requests_total{backend=\"" << b.name << "\"} " << b.requests << "\n";
    }
    os << "# TYPE edgegate_backend_errors_total counter\n";
    for (const BackendSnapshot& b : backends) {
        os << "edgegate_backend_errors_total{backend=\"" << b.name << "\"} " << b.errors << "\n";
    }
    os << "# TYPE edgegate_backend_latency_ms gauge\n";
    for (const BackendSnapshot& b : backends) {
        os << "edgegate_backend_latency_ms{backend=\"" << b.name << "\",quantile=\"0.5\"} "
           << b.latency_p50 << "\n";
        os << "edgegate_backend_latency_ms{backend=\"" << b.name << "\",quantile=\"0.95\"} "
           << b.latency_p95 << "\n";
        os << "edgegate_backend_latency_ms{backend=\"" << b.name << "\",quantile=\"0.99\"} "
           << b.latency_p99 << "\n";
    }
    return os.str();
}

}  // namespace eg
