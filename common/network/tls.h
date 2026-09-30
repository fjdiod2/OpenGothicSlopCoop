#pragma once
#include "socket.h"
#include <array>
#include <memory>
#include <string>
#include <openssl/ssl.h>

namespace Coop {
// TLS 1.3 external PSK; the invitation is 32 random bytes encoded as 64 hex digits.
class TlsContext {
  public:
    explicit TlsContext(const std::string& invitation);
    ~TlsContext();
    TlsContext(const TlsContext&)=delete;
    TlsContext& operator=(const TlsContext&)=delete;
    SSL_CTX* get() const { return context.get(); }
    const auto& key() const { return secret; }
  private:
    std::array<unsigned char,32> secret{};
    std::unique_ptr<SSL_CTX,decltype(&SSL_CTX_free)> context{nullptr,SSL_CTX_free};
  };

class TlsStream {
  public:
    TlsStream(std::shared_ptr<TlsContext> context,Net::Handle fd,bool host);
    void beginTick() { io.readLeft=io.writeLeft=65536; }
    int handshake(); // 1 authenticated, 0 pending, -1 failed
    std::ptrdiff_t read(std::span<uint8_t> bytes);
    std::ptrdiff_t write(std::span<const uint8_t> bytes);
    bool blocked() const { return waiting; }
  private:
    static int session(SSL* ssl,SSL_SESSION** output);
    static int clientKey(SSL* ssl,const EVP_MD* md,const unsigned char** id,size_t* length,SSL_SESSION** output);
    static int serverKey(SSL* ssl,const unsigned char* id,size_t length,SSL_SESSION** output);
    struct Io { Net::Handle fd; size_t readLeft=65536,writeLeft=65536; } io;
    static BIO_METHOD* method();
    bool retry(int result);
    std::shared_ptr<TlsContext> context;
    std::unique_ptr<SSL,decltype(&SSL_free)> ssl{nullptr,SSL_free};
    bool waiting=false;
  };
}
