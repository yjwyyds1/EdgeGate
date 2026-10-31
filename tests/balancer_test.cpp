#include "test_util.hpp"

#include <iostream>
#include <memory>
#include <vector>

#include "edgegate/backend.hpp"

using namespace eg;

namespace {

Config base_config() {
    Config cfg;
    cfg.unhealthy_threshold = 1;
    cfg.healthy_threshold = 1;
    cfg.passive_failure_cooldown_ms = 5000;
    cfg.max_connections_per_backend = 10;
    cfg.max_idle_connections_per_backend = 5;
    return cfg;
}

std::unique_ptr<Backend> make_backend(const Config& cfg, const std::string& name, std::uint16_t port) {
    BackendConfig bc;
    bc.name = name;
    bc.endpoint = Endpoint{"127.0.0.1", port};
    bc.url = "127.0.0.1:" + std::to_string(port);
    return std::make_unique<Backend>(bc, cfg);
}

void test_round_robin() {
    const Config cfg = base_config();
    auto a = make_backend(cfg, "a", 9001);
    auto b = make_backend(cfg, "b", 9002);
    auto c = make_backend(cfg, "c", 9003);
    std::vector<Backend*> backends{a.get(), b.get(), c.get()};
    Balancer bal("round_robin");
    const std::int64_t now = now_ms();
    REQUIRE(bal.select(backends, {}, now) == a.get());
    REQUIRE(bal.select(backends, {}, now) == b.get());
    REQUIRE(bal.select(backends, {}, now) == c.get());
    REQUIRE(bal.select(backends, {}, now) == a.get());
}

void test_skip_unhealthy_and_draining() {
    const Config cfg = base_config();
    auto a = make_backend(cfg, "a", 9001);
    auto b = make_backend(cfg, "b", 9002);
    std::vector<Backend*> backends{a.get(), b.get()};
    Balancer bal("round_robin");
    const std::int64_t now = now_ms();
    a->record_failure(now, false);
    REQUIRE(!a->healthy());
    REQUIRE(bal.select(backends, {}, now) == b.get());
    a->record_success(now);
    REQUIRE(a->healthy());
    a->set_draining(true);
    REQUIRE(bal.select(backends, {}, now) == b.get());
    a->set_draining(false);
    REQUIRE(bal.select(backends, {}, now) == a.get());
}

void test_exclude() {
    const Config cfg = base_config();
    auto a = make_backend(cfg, "a", 9001);
    auto b = make_backend(cfg, "b", 9002);
    std::vector<Backend*> backends{a.get(), b.get()};
    Balancer bal("round_robin");
    const std::int64_t now = now_ms();
    REQUIRE(bal.select(backends, {a.get()}, now) == b.get());
    REQUIRE(bal.select(backends, {a.get(), b.get()}, now) == nullptr);
}

void test_least_connections() {
    const Config cfg = base_config();
    auto a = make_backend(cfg, "a", 9001);
    auto b = make_backend(cfg, "b", 9002);
    std::vector<Backend*> backends{a.get(), b.get()};
    Balancer bal("least_conn");
    const std::int64_t now = now_ms();
    a->metrics.active_connections.set(3);
    b->metrics.active_connections.set(1);
    REQUIRE(bal.select(backends, {}, now) == b.get());
    b->metrics.active_connections.set(5);
    REQUIRE(bal.select(backends, {}, now) == a.get());
}

void test_cooldown_after_connect_failure() {
    const Config cfg = base_config();
    auto a = make_backend(cfg, "a", 9001);
    auto b = make_backend(cfg, "b", 9002);
    std::vector<Backend*> backends{a.get(), b.get()};
    Balancer bal("round_robin");
    const std::int64_t now = now_ms();
    a->record_failure(now, true);
    REQUIRE(!a->selectable(now));
    REQUIRE(bal.select(backends, {}, now) == b.get());
    REQUIRE(!a->selectable(now + 6000));
    REQUIRE(!a->healthy());
    a->record_success(now + 6000);
    REQUIRE(a->healthy());
    REQUIRE(a->selectable(now + 6000));
}

void test_backend_snapshot() {
    const Config cfg = base_config();
    auto a = make_backend(cfg, "a", 9001);
    a->metrics.requests.inc(7);
    a->metrics.errors.inc(2);
    a->metrics.active_connections.set(3);
    a->metrics.idle_connections.set(4);
    a->metrics.latency_ms.observe(12.5);
    const BackendSnapshot s = a->snapshot();
    REQUIRE_EQ(s.name, "a");
    REQUIRE_EQ(s.requests, 7u);
    REQUIRE_EQ(s.errors, 2u);
    REQUIRE_EQ(s.active_connections, 3u);
    REQUIRE_EQ(s.idle_connections, 4u);
    REQUIRE(s.latency_p50 >= 10.0);
}

}  // namespace

int main() {
    try {
        test_round_robin();
        test_skip_unhealthy_and_draining();
        test_exclude();
        test_least_connections();
        test_cooldown_after_connect_failure();
        test_backend_snapshot();
    } catch (const std::exception& e) {
        std::cerr << "balancer_test failed: " << e.what() << "\n";
        return 1;
    }
    std::cout << "balancer_test: all checks passed\n";
    return 0;
}
