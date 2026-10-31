#include "test_util.hpp"

#include <unistd.h>

#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>

#include "edgegate/config.hpp"

using namespace eg;

namespace {

std::string write_temp(const std::string& content, const char* name) {
    const std::string path = std::string("/tmp/") + name + "_" + std::to_string(::getpid()) + ".conf";
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << content;
    return path;
}

bool load_text(const std::string& content, Config& cfg, std::string& error) {
    const std::string path = write_temp(content, "eg_config");
    const bool ok = Config::load(path, cfg, error);
    std::remove(path.c_str());
    return ok;
}

void test_valid_config() {
    Config cfg;
    std::string error;
    const bool ok = load_text(R"CONF(
# comment
listen = 127.0.0.1:18080
admin_listen = off
balancer = round_robin
preserve_host = false
max_attempts = 3
client_buffer_high_watermark = 128K
client_buffer_low_watermark = 16K
upstream_buffer_high_watermark = 128K
upstream_buffer_low_watermark = 16K
max_total_buffered_bytes = 64M
health_interval_ms = 0
retry_body_buffer_bytes = 256K

[backend a]
url = http://127.0.0.1:9001
max_connections = 5
max_idle_connections = 2

[backend b]
url = 127.0.0.1:9002
)CONF",
                           cfg, error);
    REQUIRE(ok);
    REQUIRE_EQ(cfg.listen.host, "127.0.0.1");
    REQUIRE_EQ(cfg.listen.port, 18080);
    REQUIRE(!cfg.admin_enabled);
    REQUIRE_EQ(cfg.balancer, "round_robin");
    REQUIRE(!cfg.preserve_host);
    REQUIRE_EQ(cfg.max_attempts, 3);
    REQUIRE_EQ(cfg.client_buffer_high, 128u * 1024);
    REQUIRE_EQ(cfg.client_buffer_low, 16u * 1024);
    REQUIRE_EQ(cfg.max_total_buffered, 64u * 1024 * 1024);
    REQUIRE_EQ(cfg.health_interval_ms, 0);
    REQUIRE_EQ(cfg.retry_body_buffer_bytes, 256u * 1024);
    REQUIRE_EQ(cfg.backends.size(), 2u);
    REQUIRE_EQ(cfg.backends[0].name, "a");
    REQUIRE_EQ(cfg.backends[0].endpoint.port, 9001);
    REQUIRE_EQ(cfg.backends[0].max_connections, 5u);
    REQUIRE_EQ(cfg.backends[0].max_idle, 2u);
    REQUIRE_EQ(cfg.backends[1].endpoint.host, "127.0.0.1");
}

void expect_invalid(const std::string& content, const std::string& needle) {
    Config cfg;
    std::string error;
    const bool ok = load_text(content, cfg, error);
    REQUIRE(!ok);
    if (error.find(needle) == std::string::npos) {
        throw std::runtime_error("error message '" + error + "' does not contain '" + needle + "'");
    }
}

void test_invalid_configs() {
    expect_invalid("listen = 127.0.0.1:8080\n", "no [backend");
    expect_invalid("listen = 127.0.0.1:8080\nunknown_key = 1\n[backend a]\nurl=127.0.0.1:1\n",
                   "unknown key");
    expect_invalid("listen = 127.0.0.1:8080\n[backend a]\nurl=127.0.0.1:1\n"
                   "[backend a]\nurl=127.0.0.1:2\n",
                   "duplicate backend");
    expect_invalid("listen = 127.0.0.1:8080\nbalancer = random\n[backend a]\nurl=127.0.0.1:1\n",
                   "balancer");
    expect_invalid("listen = 127.0.0.1:8080\nclient_buffer_high_watermark = 16K\n"
                   "client_buffer_low_watermark = 32K\n[backend a]\nurl=127.0.0.1:1\n",
                   "low_watermark");
    expect_invalid("listen = 127.0.0.1:8080\n[backend a]\nurl=127.0.0.1:notaport\n", "port");
    expect_invalid("listen = 127.0.0.1:8080\n[backend a]\n", "requires url");
    expect_invalid("listen = 127.0.0.1:8080\nmax_attempts = 0\n[backend a]\nurl=127.0.0.1:1\n",
                   "max_attempts");
    expect_invalid("listen = 127.0.0.1:8080\n[backend a]\nurl=http://127.0.0.1:1/path\n",
                   "without a path");
}

void test_defaults() {
    Config cfg;
    std::string error;
    REQUIRE(load_text("listen = 127.0.0.1:18080\n[backend a]\nurl=127.0.0.1:9001\n", cfg, error));
    REQUIRE(cfg.admin_enabled);
    REQUIRE_EQ(cfg.admin.host, "127.0.0.1");
    REQUIRE_EQ(cfg.admin.port, 9900);
    REQUIRE_EQ(cfg.balancer, "least_conn");
    REQUIRE_EQ(cfg.max_attempts, 2);
    REQUIRE_EQ(cfg.max_request_header_bytes, 32768u);
    REQUIRE_EQ(cfg.client_buffer_high, 256u * 1024);
}

}  // namespace

int main() {
    try {
        test_valid_config();
        test_invalid_configs();
        test_defaults();
    } catch (const std::exception& e) {
        std::cerr << "config_test failed: " << e.what() << "\n";
        return 1;
    }
    std::cout << "config_test: all checks passed\n";
    return 0;
}
