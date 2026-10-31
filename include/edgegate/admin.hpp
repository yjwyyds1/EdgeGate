#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "edgegate/config.hpp"
#include "edgegate/event_loop.hpp"

namespace eg {

class ProxyServer;

// 独立的管理端口：状态 JSON、Prometheus 指标、请求追踪和一个内置 HTML 面板。
// 不复用代理转发路径，避免业务请求与运维接口互相干扰。
class AdminServer : public EventHandler {
public:
    AdminServer(ProxyServer& proxy, const Config& cfg, EventLoop& loop);
    ~AdminServer() override;

    bool init(std::string& error);
    int fd() const { return listen_fd_; }

    void on_event(int fd, std::uint32_t events) override;
    void on_tick(std::int64_t now);
    void close();

    struct Conn;

private:
    void accept_all();
    void reap();

    ProxyServer& proxy_;
    const Config& cfg_;
    EventLoop& loop_;
    int listen_fd_ = -1;
    std::unordered_map<int, std::unique_ptr<Conn>> conns_;
    std::vector<int> zombies_;
};

}  // namespace eg
