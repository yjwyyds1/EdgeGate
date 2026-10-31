#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace eg {

using Clock = std::chrono::steady_clock;

// 单调时钟，用于超时与耗时统计；不受系统时间调整影响。
std::int64_t now_ms();
// 墙钟 UTC，用于日志与展示。
std::int64_t unix_ms();
std::string iso8601_now();
std::string http_date_now();

std::string trim(std::string_view s);
std::string to_lower(std::string_view s);
bool iequals(std::string_view a, std::string_view b);
std::vector<std::string> split(std::string_view s, char sep);
std::string join(const std::vector<std::string>& parts, std::string_view sep);
bool parse_i64(std::string_view s, std::int64_t& out);
bool parse_u64(std::string_view s, std::uint64_t& out);
std::string random_hex(std::size_t bytes);
// 把任意字节串转成安全的 JSON 字符串内容（不含两侧引号）。
std::string json_escape(std::string_view s);

// ---- socket 工具（Linux/POSIX）----

struct Endpoint {
    std::string host;
    std::uint16_t port = 0;
    std::string authority() const;
};

// 接受 "host:port"、"[::1]:port"、"http://host:port"；不接受路径。
bool parse_endpoint(std::string_view text, Endpoint& out, std::string& error);

bool set_nonblocking(int fd);
bool set_cloexec(int fd);
void set_tcp_nodelay(int fd);
void set_keepalive(int fd);

// 绑定并监听，返回非阻塞 fd；失败返回 -1 并写入 error。
int create_listener(const Endpoint& ep, int backlog, std::string& error);

// 非阻塞 connect。返回 fd；-1 表示立即失败；*in_progress=true 表示需要等待 EPOLLOUT。
int connect_nonblocking(const Endpoint& ep, bool& in_progress, std::string& error);

std::string peer_ip(int fd);
std::string fd_description(int fd);

}  // namespace eg
