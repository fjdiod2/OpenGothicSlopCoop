#include "tls.h"
#include <cstring>
#include <openssl/err.h>

namespace Coop {
namespace {
constexpr char Identity[]="OpenGothic-invitation-v1";
int digit(char c) {
  if(c>='0' && c<='9') return c-'0';
  if(c>='a' && c<='f') return c-'a'+10;
  if(c>='A' && c<='F') return c-'A'+10;
  throw std::invalid_argument("Join secret must contain 64 hexadecimal digits");
  }
}
TlsContext::TlsContext(const std::string& invitation) {
  if(invitation.size()!=64) throw std::invalid_argument("Join secret must contain 64 hexadecimal digits");
  for(size_t i=0;i<secret.size();++i) secret[i]=uint8_t(digit(invitation[2*i])*16+digit(invitation[2*i+1]));
  context.reset(SSL_CTX_new(TLS_method()));
  if(!context || SSL_CTX_set_min_proto_version(get(),TLS1_3_VERSION)!=1 ||
     SSL_CTX_set_max_proto_version(get(),TLS1_3_VERSION)!=1 ||
     SSL_CTX_set_ciphersuites(get(),"TLS_AES_128_GCM_SHA256")!=1 ||
     SSL_CTX_set1_groups_list(get(),"X25519:P-256")!=1)
    throw std::runtime_error("Cannot configure TLS 1.3 invitation transport");
  SSL_CTX_set_options(get(),SSL_OP_NO_TICKET|SSL_OP_NO_RENEGOTIATION);
  SSL_CTX_set_session_cache_mode(get(),SSL_SESS_CACHE_OFF);
  SSL_CTX_set_num_tickets(get(),0);
  SSL_CTX_set_max_early_data(get(),0);
  }
TlsContext::~TlsContext() { OPENSSL_cleanse(secret.data(),secret.size()); }

BIO_METHOD* TlsStream::method() {
  static const std::unique_ptr<BIO_METHOD,decltype(&BIO_meth_free)> method([] {
    auto* value=BIO_meth_new(BIO_TYPE_SOURCE_SINK|BIO_get_new_index(),"co-op bounded socket");
    if(!value) throw std::runtime_error("Cannot allocate TLS socket method");
    BIO_meth_set_create(value,[](BIO* bio) { BIO_set_init(bio,1); return 1; });
    BIO_meth_set_destroy(value,[](BIO*) { return 1; });
    BIO_meth_set_ctrl(value,[](BIO*,int command,long,void*) -> long { return command==BIO_CTRL_FLUSH ? 1 : 0; });
    BIO_meth_set_read(value,[](BIO* bio,char* output,int size) -> int {
      auto& state=*static_cast<Io*>(BIO_get_data(bio));
      BIO_clear_retry_flags(bio);
      if(state.readLeft==0) { BIO_set_retry_read(bio); return -1; }
      const auto count=Net::receive(state.fd,{reinterpret_cast<uint8_t*>(output),std::min(size_t(size),state.readLeft)});
      if(count<0 && Net::wouldBlock()) BIO_set_retry_read(bio);
      if(count>0) state.readLeft-=size_t(count);
      return int(count);
      });
    BIO_meth_set_write(value,[](BIO* bio,const char* input,int size) -> int {
      auto& state=*static_cast<Io*>(BIO_get_data(bio));
      BIO_clear_retry_flags(bio);
      if(state.writeLeft==0) { BIO_set_retry_write(bio); return -1; }
      const auto count=Net::send(state.fd,{reinterpret_cast<const uint8_t*>(input),std::min(size_t(size),state.writeLeft)});
      if(count<0 && Net::wouldBlock()) BIO_set_retry_write(bio);
      if(count>0) state.writeLeft-=size_t(count);
      return int(count);
      });
    return value;
    }(),BIO_meth_free);
  return method.get();
  }

TlsStream::TlsStream(std::shared_ptr<TlsContext> context,Net::Handle fd,bool host):io{fd},context(std::move(context)) {
  ssl.reset(SSL_new(this->context->get()));
  if(!ssl) throw std::runtime_error("Cannot allocate TLS connection");
  auto* bio=BIO_new(method());
  if(!bio) throw std::runtime_error("Cannot allocate TLS socket");
  BIO_set_data(bio,&io);
  SSL_set_bio(ssl.get(),bio,bio);
  SSL_set_app_data(ssl.get(),this);
  if(host) {
    SSL_set_psk_find_session_callback(ssl.get(),serverKey);
    SSL_set_accept_state(ssl.get());
    }
  else {
    SSL_set_psk_use_session_callback(ssl.get(),clientKey);
    SSL_set_connect_state(ssl.get());
    }
  }
int TlsStream::session(SSL* ssl,SSL_SESSION** output) {
  auto& self=*static_cast<TlsStream*>(SSL_get_app_data(ssl));
  constexpr unsigned char cipherId[]={0x13,0x01};
  const auto* cipher=SSL_CIPHER_find(ssl,cipherId);
  auto* value=SSL_SESSION_new();
  if(!value) return 0;
  if(!cipher || SSL_SESSION_set1_master_key(value,self.context->key().data(),self.context->key().size())!=1 ||
     SSL_SESSION_set_cipher(value,cipher)!=1 || SSL_SESSION_set_protocol_version(value,TLS1_3_VERSION)!=1) {
    SSL_SESSION_free(value);
    return 0;
    }
  *output=value; // OpenSSL takes ownership, including repeated callback invocations.
  return 1;
  }
int TlsStream::clientKey(SSL* ssl,const EVP_MD* md,const unsigned char** id,size_t* length,SSL_SESSION** output) {
  if(md && EVP_MD_get_type(md)!=NID_sha256) return 0;
  *id=reinterpret_cast<const unsigned char*>(Identity);
  *length=sizeof(Identity)-1;
  return session(ssl,output);
  }
int TlsStream::serverKey(SSL* ssl,const unsigned char* id,size_t length,SSL_SESSION** output) {
  if(length!=sizeof(Identity)-1 || std::memcmp(id,Identity,length)!=0) return 0;
  return session(ssl,output);
  }
bool TlsStream::retry(int result) {
  const int code=SSL_get_error(ssl.get(),result);
  waiting=code==SSL_ERROR_WANT_READ || code==SSL_ERROR_WANT_WRITE;
  return waiting;
  }
int TlsStream::handshake() {
  ERR_clear_error();
  const int result=SSL_do_handshake(ssl.get());
  if(result==1) return SSL_version(ssl.get())==TLS1_3_VERSION ? 1 : -1;
  return retry(result) ? 0 : -1;
  }
std::ptrdiff_t TlsStream::read(std::span<uint8_t> bytes) {
  ERR_clear_error();
  size_t count=0;
  const int result=SSL_read_ex(ssl.get(),bytes.data(),bytes.size(),&count);
  if(result==1) { waiting=false; return std::ptrdiff_t(count); }
  retry(result);
  return -1;
  }
std::ptrdiff_t TlsStream::write(std::span<const uint8_t> bytes) {
  ERR_clear_error();
  size_t count=0;
  const int result=SSL_write_ex(ssl.get(),bytes.data(),std::min(bytes.size(),size_t(16384)),&count);
  if(result==1) { waiting=false; return std::ptrdiff_t(count); }
  retry(result);
  return -1;
  }
}
