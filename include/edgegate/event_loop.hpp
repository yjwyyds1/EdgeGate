#pragma once

#include <sys/epoll.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace eg {

class EventHandler {
public:
    virtual ~EventHandler() = default;
    virtual void on_event(int fd, std::uint32_t events) = 0;
};

// 单个 epoll 实例。删除 fd 与关闭 fd 分开：
// - remove(): 从 epoll 摘除但保留 fd（连接池复用）
// - close_later(): 摘除并在本批事件处理完后 close，避免 fd 号在回调中途被复用
class EventLoop {
public:
    bool init(std::string& error) {
        epfd_ = ::epoll_create1(EPOLL_CLOEXEC);
        if (epfd_ < 0) {
            error = "epoll_create1 failed";
            return false;
        }
        return true;
    }

    ~EventLoop() {
        for (int fd : close_list_) ::close(fd);
        if (epfd_ >= 0) ::close(epfd_);
    }

    bool add(int fd, std::uint32_t events, EventHandler* handler) {
        epoll_event ev{};
        ev.events = events;
        ev.data.fd = fd;
        if (::epoll_ctl(epfd_, EPOLL_CTL_ADD, fd, &ev) != 0) {
            if (errno == EEXIST) return mod(fd, events);
            return false;
        }
        handlers_[fd] = handler;
        return true;
    }

    bool mod(int fd, std::uint32_t events) {
        epoll_event ev{};
        ev.events = events;
        ev.data.fd = fd;
        return ::epoll_ctl(epfd_, EPOLL_CTL_MOD, fd, &ev) == 0;
    }

    void remove(int fd) {
        ::epoll_ctl(epfd_, EPOLL_CTL_DEL, fd, nullptr);
        handlers_.erase(fd);
    }

    void close_later(int fd) {
        if (fd < 0) return;
        remove(fd);
        close_list_.push_back(fd);
    }

    int run_once(int timeout_ms) {
        epoll_event events[256];
        const int n = ::epoll_wait(epfd_, events, 256, timeout_ms);
        if (n > 0) {
            for (int i = 0; i < n; ++i) {
                const int fd = events[i].data.fd;
                auto it = handlers_.find(fd);
                if (it == handlers_.end()) continue;
                it->second->on_event(fd, events[i].events);
            }
        }
        flush_closes();
        return n;
    }

    void flush_closes() {
        for (int fd : close_list_) ::close(fd);
        close_list_.clear();
    }

    void stop() { stop_ = true; }
    bool stopped() const { return stop_; }

private:
    int epfd_ = -1;
    bool stop_ = false;
    std::unordered_map<int, EventHandler*> handlers_;
    std::vector<int> close_list_;
};

}  // namespace eg
