#pragma once

#include <string>

#include "edgegate/http.hpp"

namespace eg {

// 重试不是"后端挂了就换一台"：只有确认请求尚未对后端产生可观察副作用时才安全。
// - 非幂等方法（POST/PATCH）在写出任何请求字节后一律不重试；
// - 幂等方法可以重试，但已转发的 body 必须能完整重放；
// - 收到任何响应字节后不再重试。
struct RetryInput {
    std::string method;
    int attempts_done = 0;
    int max_attempts = 2;
    bool idempotent_only = true;
    bool request_bytes_written = false;  // 已向任一后端写出 head 或 body
    bool body_forwarded = false;         // 已写出 body 字节
    bool body_complete = false;          // 客户端 body 已完整读取
    bool body_replayable = false;        // 完整 body 已缓存在重放缓冲区
    bool response_started = false;       // 已收到任何响应字节/响应头
};

bool can_retry(const RetryInput& input);

}  // namespace eg
