#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "edgegate/config.hpp"
#include "edgegate/log.hpp"
#include "edgegate/proxy.hpp"

namespace {

volatile sig_atomic_t g_parent_stop = 0;

void parent_on_signal(int) { g_parent_stop = 1; }

void usage(const char* argv0) {
    std::cout << "EdgeGate " << "1.3.0"
              << " - HTTP/1.1 + HTTP/2 reverse proxy with optional TLS\n\n"
              << "Usage: " << argv0 << " [options]\n\n"
              << "  --config PATH       配置文件（默认 config/edgegate.conf）\n"
              << "  --check-config      只校验配置并打印生效值，不启动监听\n"
              << "  --log-level LEVEL   debug | info | warn | error\n"
              << "  --version           打印版本\n"
              << "  --help              显示帮助\n";
}

int run_worker(eg::Config cfg, int worker_id) {
    cfg.worker_id = worker_id;
    // 只有 worker 0 开管理端，避免多进程抢同一 admin 端口。
    if (worker_id != 0) cfg.admin_enabled = false;

    eg::ProxyServer server(std::move(cfg));
    std::string error;
    if (!server.init(error)) {
        std::cerr << "worker " << worker_id << " startup failed: " << error << "\n";
        return 1;
    }
    return server.run();
}

int run_multi_worker(eg::Config cfg) {
    const int n = cfg.num_workers;
    std::vector<pid_t> kids;
    kids.reserve(static_cast<std::size_t>(n));

    for (int i = 0; i < n; ++i) {
        const pid_t pid = ::fork();
        if (pid < 0) {
            std::cerr << "fork failed: " << std::strerror(errno) << "\n";
            for (pid_t c : kids) ::kill(c, SIGTERM);
            return 1;
        }
        if (pid == 0) {
            // 子进程：各自持有 SO_REUSEPORT 监听与独立事件循环
            return run_worker(cfg, i);
        }
        kids.push_back(pid);
        eg::log_msg(eg::LogLevel::Info, "spawned worker ", i, " pid ", static_cast<long>(pid));
    }

    // 父进程：转发信号、回收子进程
    ::signal(SIGINT, parent_on_signal);
    ::signal(SIGTERM, parent_on_signal);
    ::signal(SIGHUP, parent_on_signal);
    eg::log_msg(eg::LogLevel::Info, "master pid ", static_cast<long>(::getpid()), " supervising ",
                n, " workers (SO_REUSEPORT)");

    while (!kids.empty()) {
        if (g_parent_stop) {
            for (pid_t c : kids) ::kill(c, SIGTERM);
            g_parent_stop = 0;
        }
        int status = 0;
        const pid_t done = ::waitpid(-1, &status, WNOHANG);
        if (done > 0) {
            kids.erase(std::remove(kids.begin(), kids.end(), done), kids.end());
            if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
                eg::log_msg(eg::LogLevel::Error, "worker pid ", static_cast<long>(done),
                            " exited with ", WEXITSTATUS(status));
            } else if (WIFSIGNALED(status)) {
                eg::log_msg(eg::LogLevel::Warn, "worker pid ", static_cast<long>(done),
                            " killed by signal ", WTERMSIG(status));
            }
            // 一个 worker 退出则让其余退出，避免半残集群
            if (!kids.empty() && !g_parent_stop) {
                for (pid_t c : kids) ::kill(c, SIGTERM);
            }
            continue;
        }
        if (done < 0) {
            if (errno == EINTR) continue;
            if (errno == ECHILD) break;
            eg::log_msg(eg::LogLevel::Error, "waitpid: ", std::strerror(errno));
            break;
        }
        ::usleep(100 * 1000);
    }
    return 0;
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
            std::cout << "edgegate 1.3.0\n";
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
            std::cout << "  " << b.name << " -> " << b.endpoint.authority()
                      << (b.http2 ? " (http2)" : "") << "\n";
        }
        return 0;
    }

    if (cfg.num_workers > 1) {
        return run_multi_worker(std::move(cfg));
    }
    return run_worker(std::move(cfg), 0);
}
