#pragma once

// Only this boundary knows Winsock/POSIX handle, error and buffer conventions.
#include <algorithm>
#include <cstdint>
#include <limits>
#include <span>
#include <system_error>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace Coop::Net {
#ifdef _WIN32
using Handle = SOCKET;
using Length = int;
using IoSize = int;
inline constexpr Handle Invalid = INVALID_SOCKET;
inline int error() { return WSAGetLastError(); }
inline void close(Handle fd) { if(fd!=Invalid) closesocket(fd); }
inline bool wouldBlock() {
  const int code=error();
  return code==WSAEWOULDBLOCK || code==WSAEINTR;
  }
inline bool connecting() { return error()==WSAEWOULDBLOCK || error()==WSAEINPROGRESS; }
inline void initialize() {
  struct Runtime {
    Runtime() {
      WSADATA data{};
      const int result=WSAStartup(MAKEWORD(2,2),&data);
      if(result!=0) throw std::system_error(result,std::system_category(),"Initialize Winsock 2.2");
      }
    ~Runtime() { WSACleanup(); }
    };
  static const Runtime runtime;
  }
#else
using Handle = int;
using Length = socklen_t;
using IoSize = size_t;
inline constexpr Handle Invalid = -1;
inline int error() { return errno; }
inline void close(Handle fd) { if(fd!=Invalid) ::close(fd); }
inline bool wouldBlock() { return errno==EAGAIN || errno==EWOULDBLOCK || errno==EINTR; }
inline bool connecting() { return errno==EINPROGRESS; }
inline void initialize() {}
#endif

inline std::system_error failure(const char* operation) {
  return std::system_error(error(),std::system_category(),operation);
  }
struct Socket {
  Handle fd=Invalid;
  explicit Socket(Handle fd=Invalid):fd(fd) {}
  ~Socket() { close(fd); }
  Socket(const Socket&)=delete;
  Socket& operator=(const Socket&)=delete;
  };
inline int option(Handle fd,int level,int name,int value) {
  return setsockopt(fd,level,name,reinterpret_cast<const char*>(&value),sizeof(value));
  }
inline void nonblocking(Handle fd) {
#ifdef _WIN32
  u_long value=1;
  if(ioctlsocket(fd,FIONBIO,&value)!=0) throw failure("Configure nonblocking socket");
#else
  if(fcntl(fd,F_SETFL,O_NONBLOCK)<0 || fcntl(fd,F_SETFD,FD_CLOEXEC)<0)
    throw failure("Configure nonblocking socket");
#endif
  if(option(fd,IPPROTO_TCP,TCP_NODELAY,1)!=0) throw failure("Configure TCP_NODELAY");
#ifdef SO_NOSIGPIPE
  if(option(fd,SOL_SOCKET,SO_NOSIGPIPE,1)!=0) throw failure("Configure SO_NOSIGPIPE");
#endif
  }
// 0 pending, 1 connected, -1 failed. Always polls with zero timeout.
inline int connected(Handle fd) {
#ifdef _WIN32
  WSAPOLLFD pfd{fd,POLLWRNORM,0};
  const int ready=WSAPoll(&pfd,1,0);
#else
  pollfd pfd{fd,POLLOUT,0};
  const int ready=::poll(&pfd,1,0);
#endif
  if(ready<0) return wouldBlock() ? 0 : -1;
  if(ready==0) return 0;
  int code=0;
  Length size=sizeof(code);
  if(getsockopt(fd,SOL_SOCKET,SO_ERROR,reinterpret_cast<char*>(&code),&size)!=0 || code!=0) return -1;
  return 1;
  }
inline std::ptrdiff_t receive(Handle fd,std::span<uint8_t> bytes) {
  return ::recv(fd,reinterpret_cast<char*>(bytes.data()),IoSize(std::min(bytes.size(),size_t(16384))),0);
  }
inline std::ptrdiff_t send(Handle fd,std::span<const uint8_t> bytes) {
#ifdef MSG_NOSIGNAL
  constexpr int flags=MSG_NOSIGNAL;
#else
  constexpr int flags=0;
#endif
  return ::send(fd,reinterpret_cast<const char*>(bytes.data()),IoSize(std::min(bytes.size(),size_t(16384))),flags);
  }
}
