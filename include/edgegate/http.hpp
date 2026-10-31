#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "edgegate/buffer.hpp"

namespace eg {

struct Header {
    std::string name;   // 小写
    std::string value;  // 原始大小写，已去掉首尾 OWS
};

struct HttpError {
    int status = 400;
    std::string message;
};

enum class BodyMode { None, ContentLength, Chunked, UntilClose };

struct RequestHead {
    std::string method;
    std::string target;   // 转发给后端时使用的 origin-form
    std::string version;  // "HTTP/1.1" / "HTTP/1.0"
    std::string host;
    std::vector<Header> headers;

    std::uint64_t content_length = 0;
    bool has_content_length = false;
    bool chunked = false;
    bool http10 = false;
    bool keep_alive = true;
    bool expect_continue = false;
    bool upgrade = false;
    bool absolute_form = false;
};

struct ResponseHead {
    int status = 0;
    std::string reason;
    std::string version;
    std::vector<Header> headers;

    std::uint64_t content_length = 0;
    bool has_content_length = false;
    bool chunked = false;
    bool http10 = false;
    bool keep_alive = true;
};

const std::string* find_header(const std::vector<Header>& headers, std::string_view name);
std::string header_value(const std::vector<Header>& headers, std::string_view name);
bool header_has_token(const std::vector<Header>& headers, std::string_view name,
                      std::string_view token);
std::vector<std::string> connection_tokens(const std::vector<Header>& headers);
bool is_hop_by_hop(std::string_view name, const std::vector<std::string>& connection_tokens);

bool valid_token(std::string_view s);
bool valid_request_target(std::string_view s);
bool is_idempotent_method(std::string_view method);
std::string reason_phrase(int status);

// 增量解析请求头。返回 Complete 时从头缓冲区消费掉整个头部（含空行），
// 剩余字节属于 body；body 如何读取由 BodyMode 决定。
class RequestHeadParser {
public:
    enum class Status { NeedMore, Complete, Error };
    explicit RequestHeadParser(std::size_t max_header_bytes = 40960)
        : max_header_bytes_(max_header_bytes) {}
    Status parse(Buffer& in, RequestHead& out, HttpError& err);

    BodyMode body_mode() const { return body_mode_; }
    void reset();

private:
    BodyMode body_mode_ = BodyMode::None;
    std::size_t max_header_bytes_ = 40960;
};

class ResponseHeadParser {
public:
    enum class Status { NeedMore, Complete, Error };
    explicit ResponseHeadParser(std::size_t max_header_bytes = 65536)
        : max_header_bytes_(max_header_bytes) {}
    // request_is_head=true 时按 HEAD 语义不读取响应体。
    Status parse(Buffer& in, bool request_is_head, ResponseHead& out, HttpError& err);
    BodyMode body_mode() const { return body_mode_; }
    void reset();

private:
    BodyMode body_mode_ = BodyMode::None;
    std::size_t max_header_bytes_ = 65536;
};

// chunked 解码器直接消费输入缓冲区；返回的 Data 视图在下一次向同一 Buffer
// append/reserve 之前有效。调用方应立即拷贝到目标缓冲区。
class ChunkedDecoder {
public:
    enum class Status { NeedMore, Data, Done, Error };
    Status next(Buffer& in, std::string_view& data, HttpError& err);

    bool done() const { return state_ == State::Done; }
    const std::vector<Header>& trailers() const { return trailers_; }
    void reset();

private:
    enum class State { SizeLine, Data, DataCrlf, TrailerLine, Done };
    State state_ = State::SizeLine;
    std::uint64_t remaining_ = 0;
    std::vector<Header> trailers_;
    std::size_t trailer_line_bytes_ = 0;
};

// 序列化。send_chunked 为 true 时使用 Transfer-Encoding: chunked。
std::string build_request_head(const RequestHead& req, std::string_view upstream_authority,
                               std::string_view client_ip, std::string_view request_id,
                               bool preserve_host, bool send_chunked);
std::string build_response_head(const ResponseHead& resp, std::string_view upstream_name,
                                std::string_view request_id, bool client_http10,
                                bool keep_client_alive, bool send_chunked);
std::string build_error_response(int status, std::string_view message,
                                 std::string_view request_id, bool keep_alive,
                                 bool client_http10 = false, bool head_only = false);

std::string chunk_frame(std::string_view data);
std::string last_chunk_frame();

}  // namespace eg
