#include "edgegate/http.hpp"

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <limits>
#include <optional>

#include "edgegate/util.hpp"

namespace eg {
namespace {

constexpr std::size_t kMaxRequestLine = 8192;
constexpr std::size_t kMaxHeaderCount = 200;
constexpr std::size_t kMaxHeaderName = 256;
constexpr std::size_t kMaxHeaderValue = 8192;
constexpr std::uint64_t kMaxChunkSize = 256ull * 1024 * 1024;
constexpr std::size_t kMaxChunkLine = 8192;
constexpr std::size_t kMaxTrailerBytes = 8192;

bool is_token_char(unsigned char c) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return true;
    switch (c) {
        case '!': case '#': case '$': case '%': case '&': case '\'': case '*':
        case '+': case '-': case '.': case '^': case '_': case '`': case '|': case '~':
            return true;
        default:
            return false;
    }
}

bool valid_header_value(std::string_view v) {
    for (unsigned char c : v) {
        if (c == '\r' || c == '\n' || c == 0) return false;
        if (c < 0x20 && c != '\t') return false;
        if (c == 0x7f) return false;
    }
    return true;
}

std::string trim_ows(std::string_view s) {
    std::size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t')) --e;
    return std::string(s.substr(b, e - b));
}

// 解析阶段统一小写存储；输出时恢复常见的 X-Header-Name 形式，便于阅读。
std::string canonical_header_name(std::string_view lower) {
    std::string out(lower);
    bool upper = true;
    for (char& c : out) {
        if (upper && c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
        upper = c == '-';
    }
    return out;
}

bool append_header(std::string_view raw, std::vector<Header>& out, HttpError& err,
                   std::string_view what) {
    const std::size_t colon = raw.find(':');
    if (colon == std::string_view::npos) {
        err.status = 400;
        err.message = std::string("malformed ") + std::string(what) + " header line";
        return false;
    }
    const std::string name(raw.substr(0, colon));
    if (name.empty() || name.size() > kMaxHeaderName || !valid_token(name)) {
        err.status = 400;
        err.message = "invalid header field name";
        return false;
    }
    const std::string value = trim_ows(raw.substr(colon + 1));
    if (value.size() > kMaxHeaderValue || !valid_header_value(value)) {
        err.status = 400;
        err.message = "invalid header field value";
        return false;
    }
    out.push_back(Header{to_lower(name), value});
    return true;
}

bool parse_content_lengths(const std::vector<Header>& headers, std::uint64_t& out,
                           bool& present, HttpError& err, int error_status) {
    present = false;
    std::uint64_t value = 0;
    for (const Header& h : headers) {
        if (h.name != "content-length") continue;
        for (const std::string& part : split(h.value, ',')) {
            const std::string text = trim(part);
            if (text.empty()) {
                err.status = error_status;
                err.message = "empty Content-Length";
                return false;
            }
            std::uint64_t v = 0;
            auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), v);
            if (ec != std::errc{} || ptr != text.data() + text.size()) {
                err.status = error_status;
                err.message = "invalid Content-Length";
                return false;
            }
            if (present && v != value) {
                err.status = error_status;
                err.message = "conflicting Content-Length headers";
                return false;
            }
            present = true;
            value = v;
        }
    }
    out = value;
    return true;
}

bool parse_transfer_encoding(const std::vector<Header>& headers, bool& chunked,
                             bool& present, HttpError& err, int error_status) {
    present = false;
    chunked = false;
    for (const Header& h : headers) {
        if (h.name != "transfer-encoding") continue;
        present = true;
        for (const std::string& raw : split(h.value, ',')) {
            const std::string token = to_lower(trim(raw));
            if (token.empty()) continue;
            if (token != "chunked") {
                err.status = error_status;
                err.message = "unsupported Transfer-Encoding: " + token;
                return false;
            }
            chunked = true;
        }
    }
    return true;
}

bool find_head(std::size_t start, std::size_t limit, Buffer& in, std::string_view& block,
               std::size_t& total_len, HttpError& err, std::string_view kind) {
    const std::string_view v = in.view();
    const std::size_t pos = v.find("\r\n\r\n", start);
    if (pos == std::string_view::npos) {
        if (v.size() > limit) {
            err.status = 431;
            err.message = std::string(kind) + " headers too large";
            return false;
        }
        return false;
    }
    total_len = pos + 4;
    if (total_len > limit) {
        err.status = 431;
        err.message = std::string(kind) + " headers too large";
        return false;
    }
    block = v.substr(0, pos);
    return true;
}

bool parse_request_line(std::string_view line, RequestHead& out, HttpError& err) {
    if (line.size() > kMaxRequestLine) {
        err.status = 414;
        err.message = "request line too long";
        return false;
    }
    const std::size_t sp1 = line.find(' ');
    if (sp1 == std::string_view::npos) {
        err.status = 400;
        err.message = "malformed request line";
        return false;
    }
    const std::size_t sp2 = line.find(' ', sp1 + 1);
    if (sp2 == std::string_view::npos) {
        err.status = 400;
        err.message = "malformed request line";
        return false;
    }
    out.method = std::string(line.substr(0, sp1));
    const std::string_view raw_target = line.substr(sp1 + 1, sp2 - sp1 - 1);
    out.version = std::string(line.substr(sp2 + 1));
    if (out.method.empty() || out.method.size() > 32 || !valid_token(out.method)) {
        err.status = 400;
        err.message = "invalid method";
        return false;
    }
    if (out.version != "HTTP/1.1" && out.version != "HTTP/1.0") {
        err.status = 505;
        err.message = "only HTTP/1.0 and HTTP/1.1 are supported";
        return false;
    }
    out.http10 = out.version == "HTTP/1.0";
    if (raw_target.empty() || !valid_request_target(raw_target)) {
        err.status = 400;
        err.message = "invalid request target";
        return false;
    }

    std::string target(raw_target);
    if (target.rfind("http://", 0) == 0 || target.rfind("https://", 0) == 0) {
        out.absolute_form = true;
        const std::size_t scheme_end = target.find("://") + 3;
        const std::size_t path_start = target.find_first_of("/?", scheme_end);
        const std::string authority =
            path_start == std::string::npos ? target.substr(scheme_end)
                                            : target.substr(scheme_end, path_start - scheme_end);
        if (authority.empty()) {
            err.status = 400;
            err.message = "absolute-form target has no authority";
            return false;
        }
        out.host = authority;
        if (path_start == std::string::npos) {
            out.target = "/";
        } else if (target[path_start] == '?') {
            out.target = "/" + target.substr(path_start);
        } else {
            out.target = target.substr(path_start);
        }
    } else if (target == "*") {
        if (out.method != "OPTIONS") {
            err.status = 400;
            err.message = "asterisk-form is only valid for OPTIONS";
            return false;
        }
        out.target = target;
    } else if (target.front() == '/') {
        out.target = target;
    } else if (out.method == "CONNECT") {
        out.target = target;
    } else {
        err.status = 400;
        err.message = "request target must be origin-form or absolute-form";
        return false;
    }
    return true;
}

bool parse_status_line(std::string_view line, ResponseHead& out, HttpError& err) {
    if (line.size() > kMaxRequestLine) {
        err.status = 502;
        err.message = "upstream status line too long";
        return false;
    }
    const std::size_t sp1 = line.find(' ');
    if (sp1 == std::string_view::npos) {
        err.status = 502;
        err.message = "malformed upstream status line";
        return false;
    }
    const std::size_t sp2 = line.find(' ', sp1 + 1);
    out.version = std::string(line.substr(0, sp1));
    const std::string_view code_text =
        sp2 == std::string_view::npos ? line.substr(sp1 + 1) : line.substr(sp1 + 1, sp2 - sp1 - 1);
    out.reason = sp2 == std::string_view::npos ? "" : trim(line.substr(sp2 + 1));
    if (out.version != "HTTP/1.1" && out.version != "HTTP/1.0") {
        err.status = 502;
        err.message = "unsupported upstream HTTP version";
        return false;
    }
    out.http10 = out.version == "HTTP/1.0";
    if (code_text.size() != 3) {
        err.status = 502;
        err.message = "invalid upstream status code";
        return false;
    }
    int code = 0;
    auto [ptr, ec] = std::from_chars(code_text.data(), code_text.data() + code_text.size(), code);
    if (ec != std::errc{} || ptr != code_text.data() + code_text.size() || code < 100 || code > 599) {
        err.status = 502;
        err.message = "invalid upstream status code";
        return false;
    }
    out.status = code;
    return true;
}

bool parse_header_block(std::string_view block, std::vector<Header>& headers, HttpError& err,
                        int error_status) {
    std::size_t pos = block.find("\r\n");
    std::string_view rest = pos == std::string_view::npos ? std::string_view{} : block.substr(pos + 2);
    if (pos == std::string_view::npos) return true;
    while (!rest.empty()) {
        const std::size_t eol = rest.find("\r\n");
        const std::string_view line = eol == std::string_view::npos ? rest : rest.substr(0, eol);
        if (!line.empty() && (line.front() == ' ' || line.front() == '\t')) {
            err.status = error_status;
            err.message = "obsolete header folding is not accepted";
            return false;
        }
        if (!line.empty()) {
            if (headers.size() >= kMaxHeaderCount) {
                err.status = 431;
                err.message = "too many headers";
                return false;
            }
            if (!append_header(line, headers, err, error_status == 502 ? "response" : "request")) {
                err.status = error_status;
                return false;
            }
        }
        if (eol == std::string_view::npos) break;
        rest = rest.substr(eol + 2);
    }
    return true;
}

}  // namespace

bool valid_token(std::string_view s) {
    if (s.empty()) return false;
    for (unsigned char c : s) {
        if (!is_token_char(c)) return false;
    }
    return true;
}

bool valid_request_target(std::string_view s) {
    if (s.empty() || s.size() > kMaxRequestLine) return false;
    if (s.find('#') != std::string_view::npos) return false;
    for (unsigned char c : s) {
        if (c <= 0x20 || c == 0x7f) return false;
    }
    return true;
}

bool is_idempotent_method(std::string_view method) {
    return iequals(method, "GET") || iequals(method, "HEAD") || iequals(method, "OPTIONS") ||
           iequals(method, "TRACE") || iequals(method, "PUT") || iequals(method, "DELETE");
}

const std::string* find_header(const std::vector<Header>& headers, std::string_view name) {
    for (const Header& h : headers) {
        if (h.name == name) return &h.value;
    }
    return nullptr;
}

std::string header_value(const std::vector<Header>& headers, std::string_view name) {
    const std::string* v = find_header(headers, name);
    return v ? *v : std::string();
}

std::vector<std::string> connection_tokens(const std::vector<Header>& headers) {
    std::vector<std::string> out;
    for (const Header& h : headers) {
        if (h.name != "connection") continue;
        for (const std::string& raw : split(h.value, ',')) {
            const std::string token = to_lower(trim(raw));
            if (!token.empty()) out.push_back(token);
        }
    }
    return out;
}

bool header_has_token(const std::vector<Header>& headers, std::string_view name,
                      std::string_view token) {
    for (const Header& h : headers) {
        if (h.name != name) continue;
        for (const std::string& raw : split(h.value, ',')) {
            if (iequals(trim(raw), token)) return true;
        }
    }
    return false;
}

bool is_hop_by_hop(std::string_view name, const std::vector<std::string>& tokens) {
    if (name == "connection" || name == "keep-alive" || name == "proxy-authenticate" ||
        name == "proxy-authorization" || name == "te" || name == "trailer" ||
        name == "transfer-encoding" || name == "upgrade" || name == "proxy-connection") {
        return true;
    }
    for (const std::string& t : tokens) {
        if (t == name) return true;
    }
    return false;
}

std::string reason_phrase(int status) {
    switch (status) {
        case 100: return "Continue";
        case 101: return "Switching Protocols";
        case 200: return "OK";
        case 201: return "Created";
        case 204: return "No Content";
        case 206: return "Partial Content";
        case 301: return "Moved Permanently";
        case 302: return "Found";
        case 304: return "Not Modified";
        case 307: return "Temporary Redirect";
        case 308: return "Permanent Redirect";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 408: return "Request Timeout";
        case 413: return "Payload Too Large";
        case 414: return "URI Too Long";
        case 417: return "Expectation Failed";
        case 421: return "Misdirected Request";
        case 426: return "Upgrade Required";
        case 429: return "Too Many Requests";
        case 431: return "Request Header Fields Too Large";
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        case 502: return "Bad Gateway";
        case 503: return "Service Unavailable";
        case 504: return "Gateway Timeout";
        case 505: return "HTTP Version Not Supported";
        default: return "Status";
    }
}

void RequestHeadParser::reset() { body_mode_ = BodyMode::None; }

RequestHeadParser::Status RequestHeadParser::parse(Buffer& in, RequestHead& out, HttpError& err) {
    std::string_view block;
    std::size_t head_len = 0;
    if (!find_head(0, max_header_bytes_, in, block, head_len, err, "request")) {
        if (!err.message.empty()) return Status::Error;
        return Status::NeedMore;
    }

    const std::size_t first_eol = block.find("\r\n");
    const std::string_view request_line =
        first_eol == std::string_view::npos ? block : block.substr(0, first_eol);
    if (!parse_request_line(request_line, out, err)) return Status::Error;
    if (!parse_header_block(block, out.headers, err, 400)) return Status::Error;

    std::size_t host_count = 0;
    for (const Header& h : out.headers) {
        if (h.name == "host") {
            ++host_count;
            if (h.value.empty()) {
                err = {400, "empty Host header"};
                return Status::Error;
            }
            out.host = h.value;
        }
    }
    if (host_count > 1) {
        err = {400, "multiple Host headers"};
        return Status::Error;
    }
    if (out.host.empty() && !out.absolute_form && !iequals(out.method, "CONNECT")) {
        err = {400, "HTTP/1.1 request is missing Host"};
        return Status::Error;
    }

    bool te_present = false;
    if (!parse_transfer_encoding(out.headers, out.chunked, te_present, err, 400)) return Status::Error;
    if (!parse_content_lengths(out.headers, out.content_length, out.has_content_length, err, 400)) {
        return Status::Error;
    }
    if (te_present && out.has_content_length) {
        err = {400, "Transfer-Encoding and Content-Length cannot be combined"};
        return Status::Error;
    }
    if (out.chunked && out.http10) {
        err = {400, "chunked Transfer-Encoding is not valid in HTTP/1.0"};
        return Status::Error;
    }

    out.keep_alive = !out.http10;
    for (const std::string& token : connection_tokens(out.headers)) {
        if (token == "close") out.keep_alive = false;
        if (token == "keep-alive") out.keep_alive = true;
        if (token == "upgrade") out.upgrade = true;
    }
    if (find_header(out.headers, "upgrade") != nullptr) out.upgrade = true;

    const std::string expect = header_value(out.headers, "expect");
    if (!expect.empty()) {
        bool only_continue = true;
        for (const std::string& raw : split(expect, ',')) {
            if (!iequals(trim(raw), "100-continue")) only_continue = false;
        }
        if (!only_continue) {
            err = {417, "only Expect: 100-continue is supported"};
            return Status::Error;
        }
        out.expect_continue = true;
    }

    if (out.chunked) body_mode_ = BodyMode::Chunked;
    else if (out.has_content_length && out.content_length > 0) body_mode_ = BodyMode::ContentLength;
    else body_mode_ = BodyMode::None;

    in.consume(head_len);
    return Status::Complete;
}

void ResponseHeadParser::reset() { body_mode_ = BodyMode::None; }

ResponseHeadParser::Status ResponseHeadParser::parse(Buffer& in, bool request_is_head,
                                                     ResponseHead& out, HttpError& err) {
    std::string_view block;
    std::size_t head_len = 0;
    if (!find_head(0, max_header_bytes_, in, block, head_len, err, "response")) {
        if (!err.message.empty()) return Status::Error;
        return Status::NeedMore;
    }
    const std::size_t first_eol = block.find("\r\n");
    const std::string_view status_line =
        first_eol == std::string_view::npos ? block : block.substr(0, first_eol);
    if (!parse_status_line(status_line, out, err)) return Status::Error;
    if (!parse_header_block(block, out.headers, err, 502)) return Status::Error;

    bool te_present = false;
    if (!parse_transfer_encoding(out.headers, out.chunked, te_present, err, 502)) {
        return Status::Error;
    }
    if (!parse_content_lengths(out.headers, out.content_length, out.has_content_length, err, 502)) {
        return Status::Error;
    }
    if (te_present && out.has_content_length) {
        err = {502, "upstream combined Transfer-Encoding and Content-Length"};
        return Status::Error;
    }

    out.keep_alive = !out.http10;
    for (const std::string& token : connection_tokens(out.headers)) {
        if (token == "close") out.keep_alive = false;
        if (token == "keep-alive") out.keep_alive = true;
    }

    const bool no_body =
        request_is_head || (out.status >= 100 && out.status < 200) || out.status == 204 ||
        out.status == 304;
    if (no_body) {
        body_mode_ = BodyMode::None;
    } else if (out.chunked) {
        body_mode_ = BodyMode::Chunked;
    } else if (out.has_content_length) {
        body_mode_ = out.content_length == 0 ? BodyMode::None : BodyMode::ContentLength;
    } else {
        body_mode_ = BodyMode::UntilClose;
        out.keep_alive = false;
    }

    in.consume(head_len);
    return Status::Complete;
}

void ChunkedDecoder::reset() {
    state_ = State::SizeLine;
    remaining_ = 0;
    trailers_.clear();
    trailer_line_bytes_ = 0;
}

ChunkedDecoder::Status ChunkedDecoder::next(Buffer& in, std::string_view& data, HttpError& err) {
    data = {};
    for (;;) {
        switch (state_) {
            case State::SizeLine: {
                const std::string_view v = in.view();
                const std::size_t eol = v.find("\r\n");
                if (eol == std::string_view::npos) {
                    if (v.size() > kMaxChunkLine) {
                        err = {400, "chunk size line too long"};
                        return Status::Error;
                    }
                    return Status::NeedMore;
                }
                if (eol == 0 || eol > kMaxChunkLine) {
                    err = {400, "invalid chunk size line"};
                    return Status::Error;
                }
                const std::string_view line = v.substr(0, eol);
                const std::size_t semi = line.find(';');
                const std::string_view hex = trim(line.substr(0, semi));
                if (hex.empty() || hex.size() > 16) {
                    err = {400, "invalid chunk size"};
                    return Status::Error;
                }
                std::uint64_t size = 0;
                auto [ptr, ec] = std::from_chars(hex.data(), hex.data() + hex.size(), size, 16);
                if (ec != std::errc{} || ptr != hex.data() + hex.size() || size > kMaxChunkSize) {
                    err = {400, "invalid chunk size"};
                    return Status::Error;
                }
                in.consume(eol + 2);
                remaining_ = size;
                if (size == 0) {
                    state_ = State::TrailerLine;
                } else {
                    state_ = State::Data;
                }
                break;
            }
            case State::Data: {
                if (in.empty()) return Status::NeedMore;
                const std::size_t take =
                    static_cast<std::size_t>(std::min<std::uint64_t>(remaining_, in.size()));
                data = in.view().substr(0, take);
                in.consume(take);
                remaining_ -= take;
                if (remaining_ == 0) state_ = State::DataCrlf;
                return Status::Data;
            }
            case State::DataCrlf: {
                if (in.size() < 2) return Status::NeedMore;
                const std::string_view v = in.view();
                if (v[0] != '\r' || v[1] != '\n') {
                    err = {400, "chunk data is not followed by CRLF"};
                    return Status::Error;
                }
                in.consume(2);
                state_ = State::SizeLine;
                break;
            }
            case State::TrailerLine: {
                const std::string_view v = in.view();
                const std::size_t eol = v.find("\r\n");
                if (eol == std::string_view::npos) {
                    if (trailer_line_bytes_ + v.size() > kMaxTrailerBytes) {
                        err = {400, "chunk trailers too large"};
                        return Status::Error;
                    }
                    return Status::NeedMore;
                }
                if (trailer_line_bytes_ + eol > kMaxTrailerBytes) {
                    err = {400, "chunk trailers too large"};
                    return Status::Error;
                }
                if (eol == 0) {
                    in.consume(2);
                    state_ = State::Done;
                    return Status::Done;
                }
                if (trailers_.size() >= kMaxHeaderCount) {
                    err = {400, "too many chunk trailers"};
                    return Status::Error;
                }
                if (!append_header(v.substr(0, eol), trailers_, err, "trailer")) {
                    return Status::Error;
                }
                trailer_line_bytes_ += eol + 2;
                in.consume(eol + 2);
                break;
            }
            case State::Done:
                return Status::Done;
        }
    }
}

std::string chunk_frame(std::string_view data) {
    char head[32];
    const int n = std::snprintf(head, sizeof(head), "%zx\r\n", data.size());
    std::string out;
    out.reserve(static_cast<std::size_t>(n) + data.size() + 2);
    out.append(head, static_cast<std::size_t>(n));
    out.append(data.data(), data.size());
    out.append("\r\n", 2);
    return out;
}

std::string last_chunk_frame() { return "0\r\n\r\n"; }

std::string build_request_head(const RequestHead& req, std::string_view upstream_authority,
                               std::string_view client_ip, std::string_view request_id,
                               bool preserve_host, bool send_chunked) {
    std::string out;
    out.reserve(512);
    out += req.method;
    out += ' ';
    out += req.target.empty() ? "/" : req.target;
    out += " HTTP/1.1\r\n";

    const std::vector<std::string> tokens = connection_tokens(req.headers);
    std::string forwarded_for;
    bool had_forwarded_for = false;
    for (const Header& h : req.headers) {
        if (h.name == "host" || h.name == "content-length" || h.name == "transfer-encoding" ||
            h.name == "expect" || h.name == "connection" || h.name == "proxy-connection") {
            continue;
        }
        if (h.name == "x-forwarded-for") {
            had_forwarded_for = true;
            if (!forwarded_for.empty()) forwarded_for += ", ";
            forwarded_for += h.value;
            continue;
        }
        if (is_hop_by_hop(h.name, tokens)) continue;
        out += canonical_header_name(h.name);
        out += ": ";
        out += h.value;
        out += "\r\n";
    }

    if (preserve_host && !req.host.empty()) {
        out += "Host: " + req.host + "\r\n";
    } else {
        out += "Host: ";
        out += upstream_authority;
        out += "\r\n";
    }

    if (send_chunked) {
        out += "Transfer-Encoding: chunked\r\n";
    } else if (req.has_content_length) {
        out += "Content-Length: " + std::to_string(req.content_length) + "\r\n";
    }

    if (!forwarded_for.empty()) forwarded_for += ", ";
    forwarded_for += client_ip;
    out += "X-Forwarded-For: " + forwarded_for + "\r\n";
    out += "X-Forwarded-Proto: http\r\n";
    if (!req.host.empty()) out += "X-Forwarded-Host: " + req.host + "\r\n";
    if (!request_id.empty()) out += "X-Request-Id: " + std::string(request_id) + "\r\n";
    out += "Via: 1.1 edgegate\r\n";
    out += "Connection: keep-alive\r\n";
    out += "\r\n";
    (void)had_forwarded_for;
    return out;
}

std::string build_response_head(const ResponseHead& resp, std::string_view upstream_name,
                                std::string_view request_id, bool client_http10,
                                bool keep_client_alive, bool send_chunked) {
    std::string out;
    out.reserve(512);
    out += client_http10 ? "HTTP/1.0 " : "HTTP/1.1 ";
    out += std::to_string(resp.status);
    out += ' ';
    out += resp.reason.empty() ? reason_phrase(resp.status) : resp.reason;
    out += "\r\n";

    const std::vector<std::string> tokens = connection_tokens(resp.headers);
    for (const Header& h : resp.headers) {
        if (h.name == "content-length" || h.name == "transfer-encoding" ||
            h.name == "connection" || h.name == "proxy-connection") {
            continue;
        }
        if (is_hop_by_hop(h.name, tokens)) continue;
        out += canonical_header_name(h.name);
        out += ": ";
        out += h.value;
        out += "\r\n";
    }

    if (send_chunked) {
        out += "Transfer-Encoding: chunked\r\n";
    } else if (resp.has_content_length) {
        out += "Content-Length: " + std::to_string(resp.content_length) + "\r\n";
    }

    out += keep_client_alive ? "Connection: keep-alive\r\n" : "Connection: close\r\n";
    out += "Via: 1.1 edgegate\r\n";
    if (!upstream_name.empty()) out += "X-EdgeGate-Upstream: " + std::string(upstream_name) + "\r\n";
    if (!request_id.empty())
        out += "X-EdgeGate-Request-Id: " + std::string(request_id) + "\r\n";
    out += "\r\n";
    return out;
}

std::string build_error_response(int status, std::string_view message,
                                 std::string_view request_id, bool keep_alive,
                                 bool client_http10, bool head_only) {
    std::string body = std::to_string(status);
    body += ' ';
    body += reason_phrase(status);
    if (!message.empty()) {
        body += "\n";
        body += message;
    }
    body += "\n";

    std::string out;
    out.reserve(256 + body.size());
    out += client_http10 ? "HTTP/1.0 " : "HTTP/1.1 ";
    out += std::to_string(status);
    out += ' ';
    out += reason_phrase(status);
    out += "\r\n";
    out += "Content-Type: text/plain; charset=utf-8\r\n";
    out += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    out += keep_alive ? "Connection: keep-alive\r\n" : "Connection: close\r\n";
    out += "X-EdgeGate-Error: 1\r\n";
    if (!request_id.empty())
        out += "X-EdgeGate-Request-Id: " + std::string(request_id) + "\r\n";
    out += "\r\n";
    if (!head_only) out += body;
    return out;
}

}  // namespace eg
