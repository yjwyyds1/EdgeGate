#include <iostream>
#include <string>

#include "edgegate/config.hpp"
#include "edgegate/log.hpp"
#include "edgegate/proxy.hpp"

namespace {

void usage(const char* argv0) {
    std::cout << "EdgeGate " << "1.1.0"
              << " - HTTP/1.1 + HTTP/2 reverse proxy with optional TLS\n\n"
              << "Usage: " << argv0 << " [options]\n\n"
              << "  --config PATH       配置文件（默认 config/edgegate.conf）\n"
              << "  --check-config      只校验配置并打印生效值，不启动监听\n"
              << "  --log-level LEVEL   debug | info | warn | error\n"
              << "  --version           打印版本\n"
              << "  --help              显示帮助\n";
}

}  // namespace

int main(int argc, char** argv) {
    std::string config_path = "config/edgegate.conf";
    std::string log_level = "info";
    bool check_only = false;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto next = [&](const char* name) -> std::string {
            if (i + 1 >= argc) {
                std::cerr << name << " requires a value\n";
                std::exit(2);
            }
            return argv[++i];
        };
        if (arg == "--config") config_path = next("--config");
        else if (arg.rfind("--config=", 0) == 0) config_path = arg.substr(9);
        else if (arg == "--log-level") log_level = next("--log-level");
        else if (arg.rfind("--log-level=", 0) == 0) log_level = arg.substr(12);
        else if (arg == "--check-config") check_only = true;
        else if (arg == "--version") {
            std::cout << "edgegate 1.1.0\n";
            return 0;
        } else if (arg == "--help" || arg == "-h") {
            usage(argv[0]);
            return 0;
        } else {
            std::cerr << "unknown option: " << arg << "\n";
            usage(argv[0]);
            return 2;
        }
    }

    eg::LogLevel level = eg::LogLevel::Info;
    if (!eg::parse_log_level(log_level, level)) {
        std::cerr << "invalid log level: " << log_level << "\n";
        return 2;
    }
    eg::set_log_level(level);

    eg::Config cfg;
    std::string error;
    if (!eg::Config::load(config_path, cfg, error)) {
        std::cerr << "config error: " << error << "\n";
        return 1;
    }

    std::cout << "config OK: " << cfg.summary() << "\n";
    if (check_only) {
        std::cout << "backends:\n";
        for (const eg::BackendConfig& b : cfg.backends) {
            std::cout << "  " << b.name << " -> " << b.endpoint.authority() << "\n";
        }
        return 0;
    }

    eg::ProxyServer server(cfg);
    if (!server.init(error)) {
        std::cerr << "startup failed: " << error << "\n";
        return 1;
    }
    return server.run();
}
