#include "session.h"
#include "game/playeridentity.h"

#include <algorithm>
#include <charconv>
#include <limits>
#include <random>
#include <system_error>

#ifndef _WIN32
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace Coop {
namespace {
uint16_t portNumber(const std::string& text, bool allowZero) {
  unsigned value = 0;
  const auto result = std::from_chars(text.data(),text.data()+text.size(),value);
  if(result.ec!=std::errc{} || result.ptr!=text.data()+text.size() || value>65535 || (!allowZero && value==0))
    throw std::invalid_argument("Invalid co-op port");
  return uint16_t(value);
  }

#ifndef _WIN32
const char* rejection(RejectReason reason) {
  switch(reason) {
    case RejectReason::Version:  return "protocol_mismatch";
    case RejectReason::Engine:   return "engine_mismatch";
    case RejectReason::GameData: return "data_mismatch";
    case RejectReason::Full:     return "host_full";
    case RejectReason::Session:  return "invalid_handshake";
    }
  return "invalid_handshake";
  }

struct Socket {
  int fd = -1;
  explicit Socket(int fd=-1):fd(fd) {}
  ~Socket() { if(fd>=0) ::close(fd); }
  Socket(const Socket&) = delete;
  Socket& operator=(const Socket&) = delete;
  };

bool wouldBlock() { return errno==EAGAIN || errno==EWOULDBLOCK || errno==EINTR; }

void nonblocking(int fd) {
  if(fcntl(fd,F_SETFL,O_NONBLOCK)<0 || fcntl(fd,F_SETFD,FD_CLOEXEC)<0)
    throw std::system_error(errno,std::generic_category(),"Configure co-op socket");
  const int yes = 1;
  if(setsockopt(fd,IPPROTO_TCP,TCP_NODELAY,&yes,sizeof(yes))<0)
    throw std::system_error(errno,std::generic_category(),"Configure TCP_NODELAY");
#ifdef SO_NOSIGPIPE
  if(setsockopt(fd,SOL_SOCKET,SO_NOSIGPIPE,&yes,sizeof(yes))<0)
    throw std::system_error(errno,std::generic_category(),"Configure SO_NOSIGPIPE");
#endif
  }

sockaddr_in address(uint16_t port) {
  sockaddr_in value = {};
  value.sin_family = AF_INET;
  value.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  value.sin_port = htons(port);
  return value;
  }
#endif
}

Endpoint hostEndpoint(const std::string& text) { return {Role::Host,portNumber(text,true)}; }
Endpoint joinEndpoint(const std::string& text) {
  constexpr std::string_view prefix = "127.0.0.1:";
  if(!text.starts_with(prefix))
    throw std::invalid_argument("Co-op join requires 127.0.0.1:PORT (loopback only)");
  return {Role::Guest,portNumber(text.substr(prefix.size()),false)};
  }

struct Session::Impl {
  NetworkStatus state;
  Hello identity;
  uint64_t nextGeneration = 0;
#ifndef _WIN32
  struct Peer {
    enum Stage { Connecting, Handshake, AcceptedSending, Connected, Closing } stage = Handshake;
    Socket socket;
    Decoder decoder;
    WriteQueue output;
    uint64_t started = 0;
    uint64_t generation = 0;
    bool bound = false;
    explicit Peer(int fd, uint64_t now):socket(fd),started(now) {}
    };
  std::unique_ptr<Socket> listener;
  std::vector<std::unique_ptr<Peer>> peers;

  void event(const std::string& reason) {
    state.reason = reason;
    ++state.events;
    }

  bool occupied() const {
    return std::any_of(peers.begin(),peers.end(),[](const auto& peer) {
      return peer->stage==Peer::Connected || peer->stage==Peer::AcceptedSending;
      });
    }

  void reject(Peer& peer, RejectReason reason, uint64_t now) {
    peer.output = WriteQueue{};
    if(!peer.output.enqueue({0,0,0,0,Reject{reason}}))
      throw std::runtime_error("Cannot queue co-op rejection");
    peer.stage = Peer::Closing;
    peer.started = now;
    ++state.rejected;
    event(rejection(reason));
    }

  void acceptPeers(uint64_t now) {
    for(size_t i=0; i<MaxPeers; ++i) {
      const int fd = ::accept(listener->fd,nullptr,nullptr);
      if(fd<0) {
        if(!wouldBlock()) event("accept_error");
        break;
        }
      auto peer = std::make_unique<Peer>(fd,now);
      if(peers.size()>=MaxPeers) {
        ++state.rejected;
        event("pending_limit");
        continue; // RAII closes the excess socket; never grow an unbounded queue.
        }
      nonblocking(fd);
      if(occupied()) reject(*peer,RejectReason::Full,now);
      peers.push_back(std::move(peer));
      }
    }

  bool receiveMessage(Peer& peer, const Message& message, uint64_t now) {
    if(peer.stage==Peer::Connected && state.role==Role::Guest &&
       std::holds_alternative<Welcome>(message.payload)) {
      if(peer.bound || message.generation!=peer.generation ||
         message.player!=PlayerIdentity::Companion || message.revision!=1) {
        event("invalid_binding");
        return false;
        }
      peer.bound = true;
      state.boundPlayer = message.player;
      state.bindingRevision = message.revision;
      ++state.bindings;
      event("bound");
      return true;
      }
    if(peer.stage!=Peer::Handshake) {
      event(state.role==Role::Host &&
            (std::holds_alternative<Welcome>(message.payload) ||
             (message.player!=0 && message.player!=PlayerIdentity::Companion)) ?
            "authority_violation" : "unexpected_message");
      return false; // Gameplay input remains disabled until snapshot readiness.
      }
    if(state.role==Role::Host) {
      const auto hello = std::get_if<Hello>(&message.payload);
      if(!hello) reject(peer,RejectReason::Session,now);
      else if(hello->engine!=identity.engine) reject(peer,RejectReason::Engine,now);
      else if(hello->gameData!=identity.gameData) reject(peer,RejectReason::GameData,now);
      else if(occupied()) reject(peer,RejectReason::Full,now);
      else {
        if(nextGeneration==std::numeric_limits<uint64_t>::max()) {
          reject(peer,RejectReason::Session,now);
          return true;
          }
        peer.generation = ++nextGeneration;
        if(!peer.output.enqueue({peer.generation,0,0,0,Accepted{identity}}))
          return false;
        peer.stage = Peer::AcceptedSending;
        }
      return true;
      }
    if(const auto denied = std::get_if<Reject>(&message.payload)) {
      ++state.rejected;
      state.state = "rejected";
      event(rejection(denied->reason));
      return false;
      }
    if(const auto accepted = std::get_if<Accepted>(&message.payload)) {
      if(accepted->identity.engine!=identity.engine || accepted->identity.gameData!=identity.gameData) {
        state.state = "rejected";
        ++state.rejected;
        event(accepted->identity.engine!=identity.engine ? "engine_mismatch" : "data_mismatch");
        return false;
        }
      peer.generation = message.generation;
      peer.stage = Peer::Connected;
      state.generation = peer.generation;
      state.state = "connected";
      ++state.accepted;
      event("accepted");
      return true;
      }
    event("invalid_handshake");
    return false;
    }

  bool pump(Peer& peer, uint64_t now) {
    if(peer.stage!=Peer::Connected && now-peer.started>=HandshakeTimeoutMs) {
      ++state.timeouts;
      event("timeout");
      return false;
      }
    if(peer.stage==Peer::Connecting) {
      pollfd pfd = {peer.socket.fd,POLLOUT,0};
      const int result = ::poll(&pfd,1,0);
      if(result<0) {
        if(errno==EINTR) return true;
        event("connect_error");
        return false;
        }
      if(result==0) return true;
      int error = 0;
      socklen_t length = sizeof(error);
      if(getsockopt(peer.socket.fd,SOL_SOCKET,SO_ERROR,&error,&length)<0 || error!=0) {
        event("connect_error");
        return false;
        }
      peer.stage = Peer::Handshake;
      state.state = "handshaking";
      }
    // Bound work per frame even if a local peer continuously writes.
    std::array<uint8_t,4096> buffer;
    for(size_t calls=0; calls<4 && peer.stage!=Peer::Closing; ++calls) {
      const auto count = ::recv(peer.socket.fd,buffer.data(),buffer.size(),0);
      if(count<0) {
        if(wouldBlock()) break;
        event("read_error");
        return false;
        }
      if(count==0) {
        try { peer.decoder.finish(); }
        catch(const ProtocolError&) { event("truncated_frame"); return false; }
        event("peer_closed");
        return false;
        }
      try {
        peer.decoder.feed(std::span(buffer).first(size_t(count)));
        while(auto message = peer.decoder.pop()) {
          if(!receiveMessage(peer,*message,now)) return false;
          if(peer.stage==Peer::Closing) break;
          }
        }
      catch(const ProtocolError& error) {
        if(state.role==Role::Host && peer.stage==Peer::Handshake) {
          reject(peer,error.code()==ProtocolErrorCode::Version ? RejectReason::Version : RejectReason::Session,now);
          }
        else {
          event(error.code()==ProtocolErrorCode::Version ? "protocol_mismatch" : "malformed_frame");
          return false;
          }
        }
      }
    for(size_t calls=0; calls<4 && !peer.output.front().empty(); ++calls) {
      auto bytes = peer.output.front();
#ifdef MSG_NOSIGNAL
      constexpr int flags = MSG_NOSIGNAL;
#else
      constexpr int flags = 0;
#endif
      const auto count = ::send(peer.socket.fd,bytes.data(),std::min(bytes.size(),size_t(4096)),flags);
      if(count<0 && wouldBlock()) break;
      if(count<=0) { event("write_error"); return false; }
      peer.output.consume(size_t(count));
      }
    if(peer.output.front().empty() && peer.stage==Peer::Closing) return false;
    if(peer.output.front().empty() && peer.stage==Peer::AcceptedSending) {
      peer.stage = Peer::Connected;
      state.generation = peer.generation;
      state.state = "connected";
      ++state.accepted;
      event("accepted");
      }
    return true;
    }
#endif
  };

Session::Session():impl(std::make_unique<Impl>()) {}
Session::~Session() = default;
const NetworkStatus& Session::status() const { return impl->state; }

bool Session::bindCompanion(uint64_t generation) {
#ifndef _WIN32
  auto& self = *impl;
  if(self.state.role!=Role::Host || generation==0) return false;
  for(auto& peer:self.peers) {
    if(peer->stage!=Impl::Peer::Connected || peer->generation!=generation) continue;
    if(peer->bound) return true;
    if(!peer->output.enqueue({generation,PlayerIdentity::Companion,0,1,Welcome{}}))
      return false;
    peer->bound = true;
    self.state.boundPlayer = PlayerIdentity::Companion;
    self.state.bindingRevision = 1;
    ++self.state.bindings;
    self.event("bound");
    return true;
    }
#else
  (void)generation;
#endif
  return false;
  }

void Session::invalidate(const std::string& reason) {
#ifndef _WIN32
  auto& self = *impl;
  if(self.state.role==Role::Disabled) return;
  for(const auto& peer:self.peers)
    if(peer->stage==Impl::Peer::Connected) ++self.state.disconnected;
  self.peers.clear();
  self.state.peers = 0;
  self.state.generation = self.state.boundPlayer = self.state.bindingRevision = 0;
  self.state.state = self.listener ? "listening" : "disconnected";
  self.event(reason);
#else
  (void)reason;
#endif
  }

void Session::start(Endpoint endpoint, const Hello& identity, uint64_t now) {
  impl = std::make_unique<Impl>();
  auto& self = *impl;
  self.state.role = endpoint.role;
  self.state.port = endpoint.port;
  self.identity = identity;
  if(endpoint.role==Role::Disabled) return;
#ifdef _WIN32
  (void)now;
  self.state.state = "failed";
  self.state.reason = "unsupported_platform";
#else
  try {
    std::random_device random;
    self.nextGeneration = (uint64_t(random())<<32) | uint64_t(random());
    const auto addr = address(endpoint.port);
    const int fd = ::socket(AF_INET,SOCK_STREAM,0);
    if(fd<0) throw std::system_error(errno,std::generic_category(),"Create co-op socket");
    auto socket = std::make_unique<Socket>(fd);
    nonblocking(fd);
    if(endpoint.role==Role::Host) {
      if(::bind(fd,reinterpret_cast<const sockaddr*>(&addr),sizeof(addr))<0 || ::listen(fd,int(MaxPeers))<0)
        throw std::system_error(errno,std::generic_category(),"Listen on co-op loopback socket");
      sockaddr_in bound = {};
      socklen_t length = sizeof(bound);
      if(getsockname(fd,reinterpret_cast<sockaddr*>(&bound),&length)<0)
        throw std::system_error(errno,std::generic_category(),"Read co-op port");
      self.listener = std::move(socket);
      self.state.port = ntohs(bound.sin_port);
      self.state.state = "listening";
      self.event("listening");
      }
    else {
      const int result = ::connect(fd,reinterpret_cast<const sockaddr*>(&addr),sizeof(addr));
      if(result<0 && errno!=EINPROGRESS)
        throw std::system_error(errno,std::generic_category(),"Connect to co-op host");
      auto peer = std::make_unique<Impl::Peer>(fd,now);
      socket->fd = -1;
      peer->stage = Impl::Peer::Connecting;
      if(!peer->output.enqueue({0,0,0,0,identity})) throw std::runtime_error("Cannot queue Hello");
      self.peers.push_back(std::move(peer));
      self.state.state = "connecting";
      self.event("connecting");
      }
    }
  catch(const std::exception& error) {
    self.listener.reset();
    self.peers.clear();
    self.state.state = "failed";
    self.event(std::string("startup_error: ")+error.what());
    }
#endif
  }

void Session::tick(uint64_t now) {
#ifndef _WIN32
  auto& self = *impl;
  try {
    if(self.listener) self.acceptPeers(now);
    for(size_t i=0; i<self.peers.size();) {
      auto& peer = *self.peers[i];
      const bool wasConnected = peer.stage==Impl::Peer::Connected;
      if(self.pump(peer,now)) { ++i; continue; }
      const bool connected = wasConnected || peer.stage==Impl::Peer::Connected;
      if(connected) {
        ++self.state.disconnected;
        self.state.boundPlayer = self.state.bindingRevision = 0;
        }
      self.peers.erase(self.peers.begin()+std::ptrdiff_t(i));
      if(self.state.role==Role::Guest && self.state.state!="rejected")
        self.state.state = connected ? "disconnected" : "failed";
      if(self.state.role==Role::Guest) self.state.generation = 0;
      }
    if(self.state.role==Role::Host && self.listener && !self.occupied()) {
      self.state.state = "listening";
      self.state.generation = 0;
      }
    self.state.peers = self.peers.size();
    }
  catch(const std::exception& error) {
    self.peers.clear();
    self.listener.reset();
    self.state.state = "failed";
    self.state.peers = 0;
    self.state.generation = self.state.boundPlayer = self.state.bindingRevision = 0;
    self.event(std::string("network_error: ")+error.what());
    }
#else
  (void)now;
#endif
  }

}
