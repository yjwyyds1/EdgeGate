#include "test_util.hpp"

#include <iostream>

#include "edgegate/retry.hpp"

using namespace eg;

namespace {

RetryInput base_get() {
    RetryInput in;
    in.method = "GET";
    in.attempts_done = 0;
    in.max_attempts = 2;
    in.idempotent_only = true;
    return in;
}

void test_connect_failure_allows_all_methods() {
    RetryInput in = base_get();
    in.method = "POST";
    REQUIRE(can_retry(in));  // 还没写出任何请求字节
    in.method = "PATCH";
    REQUIRE(can_retry(in));
}

void test_post_never_retried_after_bytes() {
    RetryInput in = base_get();
    in.method = "POST";
    in.request_bytes_written = true;
    REQUIRE(!can_retry(in));
    in.request_bytes_written = false;
    REQUIRE(can_retry(in));
}

void test_get_retried_after_head_or_body() {
    RetryInput in = base_get();
    in.request_bytes_written = true;
    REQUIRE(can_retry(in));
    in.body_forwarded = true;
    in.body_complete = false;
    REQUIRE(!can_retry(in));
    in.body_complete = true;
    in.body_replayable = false;
    REQUIRE(!can_retry(in));
    in.body_replayable = true;
    REQUIRE(can_retry(in));
}

void test_response_started_blocks() {
    RetryInput in = base_get();
    in.response_started = true;
    REQUIRE(!can_retry(in));
}

void test_attempt_limit() {
    RetryInput in = base_get();
    in.attempts_done = 2;
    REQUIRE(!can_retry(in));
    in.max_attempts = 3;
    REQUIRE(can_retry(in));
}

void test_unsafe_mode_allows_post() {
    RetryInput in = base_get();
    in.method = "POST";
    in.request_bytes_written = true;
    in.idempotent_only = false;
    REQUIRE(can_retry(in));
}

}  // namespace

int main() {
    try {
        test_connect_failure_allows_all_methods();
        test_post_never_retried_after_bytes();
        test_get_retried_after_head_or_body();
        test_response_started_blocks();
        test_attempt_limit();
        test_unsafe_mode_allows_post();
    } catch (const std::exception& e) {
        std::cerr << "retry_test failed: " << e.what() << "\n";
        return 1;
    }
    std::cout << "retry_test: all checks passed\n";
    return 0;
}
