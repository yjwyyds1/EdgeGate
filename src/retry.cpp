#include "edgegate/retry.hpp"

namespace eg {

bool can_retry(const RetryInput& in) {
    if (in.response_started) return false;
    if (in.attempts_done >= in.max_attempts) return false;

    const bool idempotent = is_idempotent_method(in.method);
    if (!idempotent && in.idempotent_only && in.request_bytes_written) {
        // POST/PATCH 一旦写出任何字节，后端就可能已经执行；不能重放。
        return false;
    }

    if (in.body_forwarded && !(in.body_complete && in.body_replayable)) {
        // 已经流式转发了一部分 body，却无法重放完整 body。
        return false;
    }
    return true;
}

}  // namespace eg
