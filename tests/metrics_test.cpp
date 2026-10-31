#include "test_util.hpp"

#include <iostream>
#include <string>
#include <vector>

#include "edgegate/metrics.hpp"

using namespace eg;

namespace {

void test_counters_and_gauges() {
    Counter c;
    c.inc();
    c.inc(4);
    REQUIRE_EQ(c.value(), 5u);
    Gauge g;
    g.inc();
    g.add(5);
    g.dec(2);
    REQUIRE_EQ(g.value(), 4);
    g.set(9);
    REQUIRE_EQ(g.value(), 9);
}

void test_histogram() {
    Histogram h;
    h.observe(0.5);
    h.observe(3);
    h.observe(30);
    h.observe(6000);
    REQUIRE_EQ(h.count(), 4u);
    REQUIRE(h.sum_ms() > 6000.0);
    REQUIRE_EQ(h.quantile(0.50), 5.0);
    REQUIRE_EQ(h.quantile(0.99), 5000.0);
    REQUIRE_EQ(h.quantile(1.0), 5000.0);
    h.reset();
    REQUIRE_EQ(h.count(), 0u);
    REQUIRE_EQ(h.quantile(0.5), 0.0);
}

void test_prometheus() {
    Metrics m;
    m.requests_total.inc(3);
    m.responses_total.inc(2);
    m.connect_errors.inc(1);
    m.buffered_bytes.set(123);
    m.request_duration_ms.observe(12.5);

    BackendSnapshot b;
    b.name = "app-a";
    b.url = "127.0.0.1:9001";
    b.healthy = true;
    b.draining = false;
    b.active_connections = 2;
    b.idle_connections = 3;
    b.requests = 10;
    b.errors = 1;
    b.latency_p50 = 1.0;
    b.latency_p95 = 2.0;
    b.latency_p99 = 3.0;

    const std::string text = render_prometheus(m, {b}, 42);
    REQUIRE(text.find("edgegate_uptime_seconds 42") != std::string::npos);
    REQUIRE(text.find("edgegate_requests_total 3") != std::string::npos);
    REQUIRE(text.find("edgegate_errors_total{reason=\"connect\"} 1") != std::string::npos);
    REQUIRE(text.find("edgegate_buffered_bytes 123") != std::string::npos);
    REQUIRE(text.find("edgegate_backend_healthy{backend=\"app-a\"} 1") != std::string::npos);
    REQUIRE(text.find("edgegate_backend_active_connections{backend=\"app-a\"} 2") !=
            std::string::npos);
    REQUIRE(text.find("edgegate_request_duration_ms_bucket{le=\"+Inf\"} 1") != std::string::npos);
}

}  // namespace

int main() {
    try {
        test_counters_and_gauges();
        test_histogram();
        test_prometheus();
    } catch (const std::exception& e) {
        std::cerr << "metrics_test failed: " << e.what() << "\n";
        return 1;
    }
    std::cout << "metrics_test: all checks passed\n";
    return 0;
}
