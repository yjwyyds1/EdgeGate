#pragma once

#include <openssl/ssl.h>

#include <cstddef>
#include <string>

namespace eg {

struct TlsOptions {
    std::string cert_file;
    std::string key_file;
    std::string min_version = "1.2";  // "1.2" or "1.3"
    bool http2 = true;
};

enum class IoStatus { Ok, WantRead, WantWrite, Eof, Error };

// 进程级 TLS 上下文：证书、私钥、最低版本、ALPN。
class TlsContext {
public:
    TlsContext();
    ~TlsContext();
    TlsContext(const TlsContext&) = delete;
    TlsContext& operator=(const TlsContext&) = delete;

    bool init(const TlsOptions& options, std::string& error);
    SSL_CTX* ctx() const { return ctx_; }
    bool http2_enabled() const { return http2_enabled_; }

private:
    static int alpn_select(SSL* ssl, const unsigned char** out, unsigned char* outlen,
                           const unsigned char* in, unsigned int inlen, void* arg);

    SSL_CTX* ctx_ = nullptr;
    bool http2_enabled_ = false;
};

// 单个客户端连接的 TLS 状态：非阻塞握手 + SSL_read/SSL_write。
class TlsConnection {
public:
    TlsConnection(SSL_CTX* ctx, int fd);
    ~TlsConnection();
    TlsConnection(const TlsConnection&) = delete;
    TlsConnection& operator=(const TlsConnection&) = delete;

    IoStatus handshake();
    IoStatus read(void* buffer, std::size_t size, std::size_t& got);
    IoStatus write(const void* buffer, std::size_t size, std::size_t& wrote);
    std::string alpn() const;
    void shutdown();

private:
    SSL* ssl_ = nullptr;
    bool shutdown_sent_ = false;
};

std::string tls_last_error();

}  // namespace eg
