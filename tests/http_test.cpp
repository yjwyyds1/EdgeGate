#include "test_util.hpp"

#include <iostream>
#include <string>
#include <vector>

#include "edgegate/buffer.hpp"
#include "edgegate/http.hpp"

using namespace eg;

namespace {

RequestHead parse_request_ok(const std::string& raw, std::size_t max_header = 40960) {
    Buffer b;
    b.append(raw);
    RequestHeadParser parser(max_header);
    RequestHead req;
    HttpError err;
    const auto st = parser.parse(b, req, err);
    REQUIRE(st == RequestHeadParser::Status::Complete);
    REQUIRE_EQ(err.status, 400);
    return req;
}

void expect_request_error(const std::string& raw, int status, std::size_t max_header = 1024 * 1024) {
    Buffer b;
    b.append(raw);
    RequestHeadParser parser(max_header);
    RequestHead req;
    HttpError err;
    const auto st = parser.parse(b, req, err);
    REQUIRE(st == RequestHeadParser::Status::Error);
    REQUIRE_EQ(err.status, status);
}

void test_simple_get() {
    const RequestHead req =
        parse_request_ok("GET /hello?x=1 HTTP/1.1\r\nHost: example.com\r\nUser-Agent: t\r\n\r\n");
    REQUIRE_EQ(req.method, "GET");
    REQUIRE_EQ(req.target, "/hello?x=1");
    REQUIRE_EQ(req.host, "example.com");
    REQUIRE(req.keep_alive);
    REQUIRE(!req.http10);
    REQUIRE(!req.chunked);
    REQUIRE(!req.has_content_length);
}

void test_incremental_head() {
    const std::string raw =
        "POST /upload HTTP/1.1\r\nHost: a\r\nContent-Length: 5\r\n\r\nhello";
    Buffer b;
    RequestHeadParser parser;
    RequestHead req;
    HttpError err;
    for (std::size_t i = 0; i < raw.size(); ++i) {
        b.append(raw.substr(i, 1));
        const auto st = parser.parse(b, req, err);
        if (st == RequestHeadParser::Status::NeedMore) {
            REQUIRE(i + 1 < raw.size());
        } else {
            REQUIRE(st == RequestHeadParser::Status::Complete);
            REQUIRE_EQ(i + 1, raw.size() - 5);
            b.append(raw.substr(i + 1));
            break;
        }
    }
    REQUIRE_EQ(std::string(b.view()), "hello");
    REQUIRE_EQ(req.content_length, 5u);
}

void test_chunked_request() {
    const std::string raw =
        "POST /u HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: chunked\r\n\r\n"
        "4\r\nWiki\r\n5;ext=1\r\npedia\r\n0\r\nX-Sum: 9\r\n\r\n";
    Buffer b;
    b.append(raw);
    RequestHeadParser parser;
    RequestHead req;
    HttpError err;
    REQUIRE(parser.parse(b, req, err) == RequestHeadParser::Status::Complete);
    REQUIRE(req.chunked);
    REQUIRE(parser.body_mode() == BodyMode::Chunked);

    ChunkedDecoder decoder;
    std::vector<std::string> parts;
    for (;;) {
        std::string_view data;
        const auto st = decoder.next(b, data, err);
        if (st == ChunkedDecoder::Status::NeedMore) {
            REQUIRE(false);
        }
        if (st == ChunkedDecoder::Status::Data) {
            parts.emplace_back(data);
            continue;
        }
        REQUIRE(st == ChunkedDecoder::Status::Done);
        break;
    }
    REQUIRE_EQ(parts.size(), 2u);
    REQUIRE_EQ(parts[0], "Wiki");
    REQUIRE_EQ(parts[1], "pedia");
    REQUIRE_EQ(decoder.trailers().size(), 1u);
    REQUIRE_EQ(decoder.trailers()[0].name, "x-sum");
}

void test_chunked_incremental() {
    const std::string raw =
        "POST / HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nabc\r\n0\r\n\r\n";
    Buffer b;
    RequestHeadParser parser;
    RequestHead req;
    HttpError err;
    std::size_t fed = 0;
    bool head_done = false;
    while (fed < raw.size()) {
        b.append(raw.substr(fed, 1));
        ++fed;
        const auto st = parser.parse(b, req, err);
        if (st == RequestHeadParser::Status::Complete) {
            head_done = true;
            break;
        }
        REQUIRE(st == RequestHeadParser::Status::NeedMore);
    }
    REQUIRE(head_done);

    ChunkedDecoder decoder;
    std::string body;
    for (;;) {
        std::string_view data;
        const auto st = decoder.next(b, data, err);
        if (st == ChunkedDecoder::Status::NeedMore) {
            REQUIRE(fed < raw.size());
            b.append(raw.substr(fed, 1));
            ++fed;
            continue;
        }
        if (st == ChunkedDecoder::Status::Error) REQUIRE(false);
        if (st == ChunkedDecoder::Status::Done) break;
        body.append(data);
    }
    REQUIRE_EQ(body, "abc");
}

void test_request_errors() {
    expect_request_error("GET / HTTP/1.1\r\n\r\n", 400);  // missing Host
    expect_request_error("GET / HTTP/1.1\r\nHost: a\r\nContent-Length: 2\r\n"
                         "Transfer-Encoding: chunked\r\n\r\n",
                         400);
    expect_request_error("GET / HTTP/1.1\r\nHost: a\r\nHost: b\r\n\r\n", 400);
    expect_request_error("GET / HTTP/1.1\r\nHost: a\r\nContent-Length: 2\r\n"
                         "Content-Length: 3\r\n\r\n",
                         400);
    expect_request_error("GET / HTTP/1.1\r\nHost: a\r\n X: folded\r\n\r\n", 400);
    expect_request_error("GET / HTTP/9.9\r\nHost: a\r\n\r\n", 505);
    expect_request_error("GET / HTTP/1.1\r\nHost: a\r\nContent-Length: nope\r\n\r\n", 400);
    expect_request_error("GET / HTTP/1.1\r\nHost: a\r\nBad Name: x\r\n\r\n", 400);
    expect_request_error("GET / HTTP/1.1\r\nHost: a\r\nX: a\r\nb\r\n\r\n", 400);
    expect_request_error("GET / HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: gzip\r\n\r\n", 400);
}

void test_absolute_form() {
    const RequestHead req = parse_request_ok(
        "GET http://example.com/a/b?q=1 HTTP/1.1\r\nHost: example.com\r\n\r\n");
    REQUIRE(req.absolute_form);
    REQUIRE_EQ(req.host, "example.com");
    REQUIRE_EQ(req.target, "/a/b?q=1");
}

void test_expect_continue() {
    const RequestHead req =
        parse_request_ok("POST / HTTP/1.1\r\nHost: a\r\nExpect: 100-continue\r\n"
                         "Content-Length: 1\r\n\r\nx");
    REQUIRE(req.expect_continue);
}

void test_response_bodies() {
    {
        Buffer b;
        b.append("HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\nabc");
        ResponseHeadParser p;
        ResponseHead r;
        HttpError e;
        REQUIRE(p.parse(b, false, r, e) == ResponseHeadParser::Status::Complete);
        REQUIRE_EQ(r.status, 200);
        REQUIRE(p.body_mode() == BodyMode::ContentLength);
        REQUIRE_EQ(std::string(b.view()), "abc");
    }
    {
        Buffer b;
        b.append("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nabc\r\n0\r\n\r\n");
        ResponseHeadParser p;
        ResponseHead r;
        HttpError e;
        REQUIRE(p.parse(b, false, r, e) == ResponseHeadParser::Status::Complete);
        REQUIRE(p.body_mode() == BodyMode::Chunked);
        ChunkedDecoder d;
        std::string_view data;
        REQUIRE(d.next(b, data, e) == ChunkedDecoder::Status::Data);
        REQUIRE_EQ(std::string(data), "abc");
        REQUIRE(d.next(b, data, e) == ChunkedDecoder::Status::Done);
    }
    {
        Buffer b;
        b.append("HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\n");
        ResponseHeadParser p;
        ResponseHead r;
        HttpError e;
        REQUIRE(p.parse(b, true, r, e) == ResponseHeadParser::Status::Complete);
        REQUIRE(p.body_mode() == BodyMode::None);  // HEAD
    }
    {
        Buffer b;
        b.append("HTTP/1.1 204 No Content\r\n\r\n");
        ResponseHeadParser p;
        ResponseHead r;
        HttpError e;
        REQUIRE(p.parse(b, false, r, e) == ResponseHeadParser::Status::Complete);
        REQUIRE(p.body_mode() == BodyMode::None);
    }
    {
        Buffer b;
        b.append("HTTP/1.1 200 OK\r\nConnection: close\r\n\r\nbody");
        ResponseHeadParser p;
        ResponseHead r;
        HttpError e;
        REQUIRE(p.parse(b, false, r, e) == ResponseHeadParser::Status::Complete);
        REQUIRE(p.body_mode() == BodyMode::UntilClose);
        REQUIRE(!r.keep_alive);
    }
    {
        Buffer b;
        b.append("HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 201 Created\r\nContent-Length: 0\r\n\r\n");
        ResponseHeadParser p;
        ResponseHead r;
        HttpError e;
        REQUIRE(p.parse(b, false, r, e) == ResponseHeadParser::Status::Complete);
        REQUIRE_EQ(r.status, 100);
        REQUIRE(p.parse(b, false, r, e) == ResponseHeadParser::Status::Complete);
        REQUIRE_EQ(r.status, 201);
    }
}

void test_header_utils() {
    Buffer b;
    b.append("GET / HTTP/1.1\r\nHost: a\r\nConnection: close, X-Hop\r\nX-Hop: 1\r\n"
             "X-Keep: v\r\n\r\n");
    RequestHeadParser p;
    RequestHead r;
    HttpError e;
    REQUIRE(p.parse(b, r, e) == RequestHeadParser::Status::Complete);
    REQUIRE(!r.keep_alive);
    const auto tokens = connection_tokens(r.headers);
    REQUIRE(is_hop_by_hop("x-hop", tokens));
    REQUIRE(!is_hop_by_hop("x-keep", tokens));
    REQUIRE_EQ(header_value(r.headers, "x-keep"), "v");
    REQUIRE(header_has_token(r.headers, "connection", "close"));
}

void test_serialize_request() {
    Buffer b;
    b.append("POST /api?q=1 HTTP/1.1\r\nHost: example.com\r\nContent-Length: 3\r\n"
             "Connection: close, X-Hop\r\nX-Hop: secret\r\nX-Forwarded-For: 1.2.3.4\r\n"
             "X-Custom: yes\r\n\r\nabc");
    RequestHeadParser p;
    RequestHead r;
    HttpError e;
    REQUIRE(p.parse(b, r, e) == RequestHeadParser::Status::Complete);
    const std::string head = build_request_head(r, "up:9000", "5.6.7.8", "req-1", false, false);
    REQUIRE(head.find("POST /api?q=1 HTTP/1.1\r\n") == 0);
    REQUIRE(head.find("Host: up:9000\r\n") != std::string::npos);
    REQUIRE(head.find("X-Custom: yes\r\n") != std::string::npos);
    REQUIRE(head.find("X-Hop") == std::string::npos);
    REQUIRE(head.find("X-Forwarded-For: 1.2.3.4, 5.6.7.8\r\n") != std::string::npos);
    REQUIRE(head.find("Content-Length: 3\r\n") != std::string::npos);
    REQUIRE(head.find("Connection: keep-alive\r\n") != std::string::npos);
    REQUIRE(head.find("Connection: close") == std::string::npos);
}

void test_serialize_response() {
    Buffer b;
    b.append("HTTP/1.1 200 OK\r\nConnection: close\r\nSet-Cookie: a=1\r\nSet-Cookie: b=2\r\n"
             "Content-Length: 99\r\nX-Hop: gone\r\n\r\n");
    ResponseHeadParser p;
    ResponseHead r;
    HttpError e;
    REQUIRE(p.parse(b, false, r, e) == ResponseHeadParser::Status::Complete);
    const std::string head = build_response_head(r, "app-a", "req-2", false, true, true);
    REQUIRE(head.find("HTTP/1.1 200 OK\r\n") == 0);
    REQUIRE(head.find("Transfer-Encoding: chunked\r\n") != std::string::npos);
    REQUIRE(head.find("Content-Length") == std::string::npos);
    REQUIRE(head.find("Connection: close") == std::string::npos);
    REQUIRE(head.find("Set-Cookie: a=1") != std::string::npos);
    REQUIRE(head.find("Set-Cookie: b=2") != std::string::npos);
    REQUIRE(head.find("X-EdgeGate-Upstream: app-a") != std::string::npos);
}

void test_chunk_frame() {
    REQUIRE_EQ(chunk_frame("abc"), "3\r\nabc\r\n");
    REQUIRE_EQ(chunk_frame(""), "0\r\n\r\n");
    REQUIRE_EQ(last_chunk_frame(), "0\r\n\r\n");
}

void test_oversized_head() {
    std::string raw = "GET / HTTP/1.1\r\nHost: a\r\n";
    raw += "X-Big: " + std::string(2000, 'x') + "\r\n\r\n";
    expect_request_error(raw, 431, 1024);
}

void test_error_response_head_only() {
    const std::string full = build_error_response(502, "bad", "req-3", false, false, false);
    const std::string head_only = build_error_response(502, "bad", "req-3", false, false, true);
    const std::size_t end = head_only.find("\r\n\r\n");
    REQUIRE(end != std::string::npos);
    REQUIRE(head_only.substr(end + 4).empty());
    REQUIRE(full.size() > head_only.size());
    REQUIRE(head_only.find("Content-Length:") != std::string::npos);
}

}  // namespace

int main() {
    try {
        test_simple_get();
        test_incremental_head();
        test_chunked_request();
        test_chunked_incremental();
        test_request_errors();
        test_absolute_form();
        test_expect_continue();
        test_response_bodies();
        test_header_utils();
        test_serialize_request();
        test_serialize_response();
        test_chunk_frame();
        test_oversized_head();
        test_error_response_head_only();
    } catch (const std::exception& e) {
        std::cerr << "http_test failed: " << e.what() << "\n";
        return 1;
    }
    std::cout << "http_test: all checks passed\n";
    return 0;
}
