#include "edgegate/tls.hpp"

#include <openssl/err.h>

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstring>

namespace eg {
namespace {

constexpr unsigned char kAlpnH2H1[] = {2, 'h', '2', 8, 'h', 't', 't', 'p', '/', '1', '.', '1'};
constexpr unsigned char kAlpnH1[] = {8, 'h', 't', 't', 'p', '/', '1', '.', '1'};

std::string openssl_reason() {
    const unsigned long code = ERR_peek_last_error();
    if (code == 0) return "unknown TLS error";
    char buf[256];
    ERR_error_string_n(code, buf, sizeof(buf));
    return buf;
}

}  // namespace

std::string tls_last_error() { return openssl_reason(); }

TlsContext::TlsContext() = default;

TlsContext::~TlsContext() {
    if (ctx_) SSL_CTX_free(ctx_);
}

int TlsContext::alpn_select(SSL*, const unsigned char** out, unsigned char* outlen,
                            const unsigned char* in, unsigned int inlen, void* arg) {
    auto* self = static_cast<TlsContext*>(arg);
    const unsigned char* server = self && self->http2_enabled_ ? kAlpnH2H1 : kAlpnH1;
    const unsigned int server_len =
        self && self->http2_enabled_ ? sizeof(kAlpnH2H1) : sizeof(kAlpnH1);
    unsigned char* selected = nullptr;
    if (SSL_select_next_proto(&selected, outlen, server, server_len, in, inlen) !=
        OPENSSL_NPN_NEGOTIATED) {
        return SSL_TLSEXT_ERR_NOACK;
    }
    *out = selected;
    return SSL_TLSEXT_ERR_OK;
}

bool TlsContext::init(const TlsOptions& options, std::string& error) {
    ctx_ = SSL_CTX_new(TLS_server_method());
    if (!ctx_) {
        error = "SSL_CTX_new: " + openssl_reason();
        return false;
    }
    const int version = options.min_version == "1.3" ? TLS1_3_VERSION : TLS1_2_VERSION;
    if (SSL_CTX_set_min_proto_version(ctx_, version) != 1) {
        error = "SSL_CTX_set_min_proto_version: " + openssl_reason();
        return false;
    }
    SSL_CTX_set_options(ctx_, SSL_OP_NO_COMPRESSION);
    SSL_CTX_set_mode(ctx_, SSL_MODE_ENABLE_PARTIAL_WRITE |
                               SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
    http2_enabled_ = options.http2;
    SSL_CTX_set_alpn_select_cb(ctx_, &TlsContext::alpn_select, this);

    if (SSL_CTX_use_certificate_chain_file(ctx_, options.cert_file.c_str()) != 1) {
        error = "load certificate " + options.cert_file + ": " + openssl_reason();
        return false;
    }
    if (SSL_CTX_use_PrivateKey_file(ctx_, options.key_file.c_str(), SSL_FILETYPE_PEM) != 1) {
        error = "load private key " + options.key_file + ": " + openssl_reason();
        return false;
    }
    if (SSL_CTX_check_private_key(ctx_) != 1) {
        error = "certificate/private key mismatch: " + openssl_reason();
        return false;
    }
    return true;
}

TlsConnection::TlsConnection(SSL_CTX* ctx, int fd) {
    if (!ctx) return;
    ssl_ = SSL_new(ctx);
    if (!ssl_) return;
    SSL_set_fd(ssl_, fd);
    SSL_set_accept_state(ssl_);
    SSL_set_mode(ssl_, SSL_MODE_ENABLE_PARTIAL_WRITE |
                            SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
#ifdef SSL_OP_NO_RENEGOTIATION
    SSL_set_options(ssl_, SSL_OP_NO_RENEGOTIATION);
#endif
}

TlsConnection::~TlsConnection() {
    if (ssl_) SSL_free(ssl_);
}

IoStatus TlsConnection::handshake() {
    if (!ssl_) return IoStatus::Error;
    const int rc = SSL_accept(ssl_);
    if (rc == 1) return IoStatus::Ok;
    switch (SSL_get_error(ssl_, rc)) {
        case SSL_ERROR_WANT_READ: return IoStatus::WantRead;
        case SSL_ERROR_WANT_WRITE: return IoStatus::WantWrite;
        default: return IoStatus::Error;
    }
}

IoStatus TlsConnection::read(void* buffer, std::size_t size, std::size_t& got) {
    got = 0;
    if (!ssl_) return IoStatus::Error;
    const int n = SSL_read(ssl_, buffer, static_cast<int>(std::min<std::size_t>(size, INT_MAX)));
    if (n > 0) {
        got = static_cast<std::size_t>(n);
        return IoStatus::Ok;
    }
    const int e = SSL_get_error(ssl_, n);
    switch (e) {
        case SSL_ERROR_WANT_READ: return IoStatus::WantRead;
        case SSL_ERROR_WANT_WRITE: return IoStatus::WantWrite;
        case SSL_ERROR_ZERO_RETURN: return IoStatus::Eof;
        case SSL_ERROR_SYSCALL:
            if (errno == EAGAIN || errno == EWOULDBLOCK) return IoStatus::WantRead;
            if (errno == 0) return IoStatus::Eof;
            return IoStatus::Error;
        default: return IoStatus::Error;
    }
}

IoStatus TlsConnection::write(const void* buffer, std::size_t size, std::size_t& wrote) {
    wrote = 0;
    if (!ssl_) return IoStatus::Error;
    const int n = SSL_write(ssl_, buffer, static_cast<int>(std::min<std::size_t>(size, INT_MAX)));
    if (n > 0) {
        wrote = static_cast<std::size_t>(n);
        return IoStatus::Ok;
    }
    const int e = SSL_get_error(ssl_, n);
    switch (e) {
        case SSL_ERROR_WANT_READ: return IoStatus::WantRead;
        case SSL_ERROR_WANT_WRITE: return IoStatus::WantWrite;
        case SSL_ERROR_ZERO_RETURN: return IoStatus::Eof;
        case SSL_ERROR_SYSCALL:
            if (errno == EAGAIN || errno == EWOULDBLOCK) return IoStatus::WantWrite;
            if (errno == 0) return IoStatus::Eof;
            return IoStatus::Error;
        default: return IoStatus::Error;
    }
}

std::string TlsConnection::alpn() const {
    if (!ssl_) return {};
    const unsigned char* data = nullptr;
    unsigned int len = 0;
    SSL_get0_alpn_selected(ssl_, &data, &len);
    if (!data || len == 0) return {};
    return std::string(reinterpret_cast<const char*>(data), len);
}

void TlsConnection::shutdown() {
    if (!ssl_ || shutdown_sent_) return;
    shutdown_sent_ = true;
    // 只发一次 close_notify；不等待对端，避免慢客户端拖住关闭。
    SSL_shutdown(ssl_);
}

}  // namespace eg
