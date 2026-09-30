#pragma once
#include "socket.h"
#include <atomic>
#include <memory>
#include <string>
#include <thread>
#ifndef _WIN32
#include <netdb.h>
#endif

namespace Coop::Net {
struct Resolution {
  std::atomic_bool ready=false;
  sockaddr_in address{};
  std::string error;
  };
// OS DNS may outlive a session's deadline. Workers own only their result, never
// the game/session. Bound concurrent DNS calls; abandoning one never joins it.
inline std::shared_ptr<Resolution> resolve(const std::string& name,uint16_t port) {
  static auto active=std::make_shared<std::atomic_uint>(0);
  auto count=active;
  if(count->fetch_add(1)>=4) {
    --*count;
    throw std::runtime_error("DNS resolver busy; retry after pending lookups finish");
    }
  auto result=std::make_shared<Resolution>();
  try {
    std::thread([result,count,name,port] {
      try {
        addrinfo hints{};
        hints.ai_family=AF_INET;
        hints.ai_socktype=SOCK_STREAM;
        addrinfo* addresses=nullptr;
        const int code=getaddrinfo(name.c_str(),nullptr,&hints,&addresses);
        if(code!=0) result->error="Cannot resolve IPv4 hostname (DNS error "+std::to_string(code)+")";
        else {
          std::unique_ptr<addrinfo,decltype(&freeaddrinfo)> owner(addresses,freeaddrinfo);
          if(!addresses || addresses->ai_addrlen<sizeof(sockaddr_in)) result->error="Hostname has no IPv4 address";
          else {
            result->address=*reinterpret_cast<const sockaddr_in*>(addresses->ai_addr);
            result->address.sin_port=htons(port);
            }
          }
        }
      catch(...) { result->error="DNS lookup failed"; }
      result->ready.store(true,std::memory_order_release);
      --*count;
      }).detach();
    }
  catch(...) { --*count; throw; }
  return result;
  }
}
