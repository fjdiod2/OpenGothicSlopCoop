#include "session.h"
#include "fingerprint.h"
#include "game/playeridentity.h"

#include <algorithm>
#include <charconv>
#include <limits>
#include <random>
#include <system_error>

#include "socket.h"
#include "resolve.h"
#include "tls.h"

namespace Coop {
namespace {
using Net::Socket;
using Net::nonblocking;
using Net::wouldBlock;
uint16_t portNumber(const std::string& text, bool allowZero) {
  unsigned value = 0;
  const auto result = std::from_chars(text.data(),text.data()+text.size(),value);
  if(result.ec!=std::errc{} || result.ptr!=text.data()+text.size() || value>65535 || (!allowZero && value==0))
    throw std::invalid_argument("Invalid co-op port");
  return uint16_t(value);
  }

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

sockaddr_in address(uint16_t port, const std::string& host="127.0.0.1") {
  sockaddr_in value = {};
  value.sin_family = AF_INET;
  if(inet_pton(AF_INET,host.c_str(),&value.sin_addr)!=1)
    throw std::invalid_argument("Listen address must be an IPv4 literal");
  value.sin_port = htons(port);
  return value;
  }
}

Endpoint hostEndpoint(const std::string& text) {
  const auto colon=text.find(':');
  if(colon==std::string::npos) return {Role::Host,portNumber(text,true)};
  const auto host=text.substr(0,colon);
  const auto port=portNumber(text.substr(colon+1),true);
  address(port,host); // Host resolution must never silently choose an interface.
  return {Role::Host,port,host};
  }
Endpoint joinEndpoint(const std::string& text) {
  const auto colon=text.find(':');
  if(colon==std::string::npos || colon==0 || colon>253)
    throw std::invalid_argument("Co-op join requires IPv4-or-hostname:PORT");
  const auto host=text.substr(0,colon);
  bool numeric=true;
  for(char c:host) {
    if(!((c>='a' && c<='z') || (c>='A' && c<='Z') || (c>='0' && c<='9') || c=='.' || c=='-'))
      throw std::invalid_argument("Invalid co-op hostname");
    if((c<'0' || c>'9') && c!='.') numeric=false;
    }
  if(host.front()=='.' || host.front()=='-' || host.back()=='-' || host.find("..")!=std::string::npos)
    throw std::invalid_argument("Invalid co-op hostname");
  const auto port=portNumber(text.substr(colon+1),false);
  if(numeric) {
    const auto ip=ntohl(address(port,host).sin_addr.s_addr);
    if(ip==0 || ip>=0xe0000000u) throw std::invalid_argument("Invalid co-op destination IPv4");
    }
  return {Role::Guest,port,host};
  }

std::string statusDescription(const NetworkStatus& status) {
  const auto& reason=status.reason;
  if(reason=="authentication_failed") return "Invitation rejected. Check the join secret.";
  if(reason=="engine_mismatch") return "Build mismatch. Rebuild the same fork revision.";
  if(reason=="protocol_mismatch") return "Protocol mismatch. Update both game builds.";
  if(reason=="data_mismatch") return "Data mismatch. Match edition, language and mods.";
  if(reason=="host_full") return "Host already has a connected companion.";
  if(reason=="dns_timeout") return "Address lookup timed out. Check the hostname.";
  if(reason=="timeout") return "Connection timed out. Check address and port.";
  if(reason=="snapshot_timeout") return "World transfer timed out. Retry joining.";
  if(reason=="connect_error") return "Cannot reach host. Check address and forwarding.";
  if(status.state=="failed") return "Connection failed. See the launcher log.";
  if(status.state=="disconnected") return "Connection lost. Press R to rejoin.";
  if(status.state=="resolving") return "Looking up the host address...";
  if(status.state=="connecting") return "Connecting to the host...";
  if(status.state=="authenticating") return "Checking your invitation...";
  if(status.state=="listening") return "Waiting for the invited companion.";
  if(status.snapshotState!="ready") return "Waiting for the host world...";
  return "Connected.";
  }

struct Session::Impl {
  NetworkStatus state;
  Hello identity;
  uint64_t nextGeneration = 0;
  uint64_t now = 0, resolveStarted = 0;
  std::shared_ptr<Net::Resolution> resolution;
  std::shared_ptr<TlsContext> tlsContext;
  struct Peer {
    enum Stage { Connecting, Authenticating, Handshake, AcceptedSending, Connected, Closing } stage = Handshake;
    Socket socket;
    std::unique_ptr<TlsStream> tls;
    Decoder decoder;
    WriteQueue output;
    uint64_t started = 0;
    uint64_t generation = 0;
    bool bound = false;
    enum Transfer { None, Sending, Receiving, Available, Loading, AwaitReady, Ready } transfer = None;
    Bytes snapshot;
    uint64_t revision = 0, total = 0, offset = 0, deadline = 0;
    Fingerprint digest = {};
    uint64_t receivedInput = 0, appliedInput = 0;
    std::deque<Message> inputs;
    std::deque<Message> poses;
    uint64_t poseInput = 0;
    explicit Peer(Net::Handle fd, uint64_t now):socket(fd),started(now) {}
    };
  std::unique_ptr<Socket> listener;
  std::vector<std::unique_ptr<Peer>> peers;

  void connectTo(const sockaddr_in& addr,uint64_t started) {
    if(!tlsContext && (ntohl(addr.sin_addr.s_addr)>>24)!=127)
      throw std::runtime_error("External connections require a join secret");
    auto socket=std::make_unique<Socket>(::socket(AF_INET,SOCK_STREAM,0));
    if(socket->fd==Net::Invalid) throw Net::failure("Create co-op socket");
    nonblocking(socket->fd);
    const int result=::connect(socket->fd,reinterpret_cast<const sockaddr*>(&addr),sizeof(addr));
    if(result<0 && !Net::connecting()) throw Net::failure("Connect to co-op host");
    auto peer=std::make_unique<Peer>(socket->fd,started);
    socket->fd=Net::Invalid;
    if(tlsContext) peer->tls=std::make_unique<TlsStream>(tlsContext,peer->socket.fd,false);
    peer->stage=Peer::Connecting;
    if(!peer->output.enqueue({0,0,0,0,identity})) throw std::runtime_error("Cannot queue Hello");
    peers.push_back(std::move(peer));
    state.state="connecting";
    event("connecting");
    }

  void resetSnapshot() {
    state.snapshotState = "none";
    state.snapshotRevision = state.snapshotBytes = state.snapshotTransferred = 0;
    state.snapshotDigest = {};
    state.inputSent = state.inputReceived = state.inputApplied = state.inputAcknowledged = 0;
    state.acknowledgedRevision = 0;
    state.posesSent = state.posesReceived = state.posesApplied = state.posesAcknowledged = state.poseTick = 0;
    }

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
      const auto fd = ::accept(listener->fd,nullptr,nullptr);
      if(fd==Net::Invalid) {
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
      if(tlsContext) {
        peer->tls=std::make_unique<TlsStream>(tlsContext,fd,true);
        peer->stage=Peer::Authenticating;
        }
      else if(occupied()) reject(*peer,RejectReason::Full,now);
      peers.push_back(std::move(peer));
      }
    }

  bool receiveMessage(Peer& peer, const Message& message, uint64_t now) {
    const auto type = messageType(message);
    if(type==MessageType::Poses || type==MessageType::PoseAck) {
      if(peer.stage!=Peer::Connected || peer.transfer!=Peer::Ready ||
         message.generation!=peer.generation || message.player!=PlayerIdentity::Companion) {
        event("invalid_pose_session"); return false;
        }
      if(auto poses = std::get_if<Poses>(&message.payload)) {
        if(state.role!=Role::Guest || message.revision!=std::max(state.snapshotRevision,state.posesReceived)+1 ||
           poses->tick<state.poseTick || poses->appliedInput<peer.poseInput || poses->appliedInput>state.inputSent ||
           peer.poses.size()>=64) {
          event("invalid_pose_order"); return false;
          }
        peer.poses.push_back(message);
        state.posesReceived = message.revision;
        state.poseTick = poses->tick;
        peer.poseInput = poses->appliedInput;
        return true;
        }
      if(state.role!=Role::Host || message.revision<=state.snapshotRevision ||
         message.revision<state.posesAcknowledged || message.revision>state.posesSent) {
        event("invalid_pose_ack"); return false;
        }
      state.posesAcknowledged = message.revision;
      return true;
      }
    if(peer.stage==Peer::Connected && peer.bound && peer.transfer==Peer::Ready &&
       state.role==Role::Host && type==MessageType::Input) {
      if(message.player!=PlayerIdentity::Companion) { event("authority_violation"); return false; }
      if(message.generation!=peer.generation) { ++state.inputStale; return true; }
      if(message.sequence<=peer.receivedInput) { ++state.inputDuplicates; return true; }
      if(peer.receivedInput==std::numeric_limits<uint64_t>::max() || message.sequence!=peer.receivedInput+1) {
        event("input_sequence_gap"); return false;
        }
      if((std::get<Input>(message.payload).buttons & ~uint32_t(0x3f|Attack|Melee|Jump|Sneak))!=0) {
        event("unsupported_input"); return false;
        }
      if(peer.inputs.size()>=64) { event("input_queue_full"); return false; }
      peer.inputs.push_back(message);
      peer.receivedInput = state.inputReceived = message.sequence;
      return true;
      }
    if(peer.stage==Peer::Connected && peer.transfer==Peer::Ready &&
       state.role==Role::Guest && type==MessageType::StateAck) {
      const auto sequence = std::get<StateAck>(message.payload).appliedInput;
      if(message.generation!=peer.generation || message.player!=PlayerIdentity::Companion ||
         sequence<state.inputAcknowledged || sequence>state.inputSent ||
         message.revision<state.acknowledgedRevision) {
        event("invalid_input_ack"); return false;
        }
      state.inputAcknowledged = sequence;
      state.acknowledgedRevision = message.revision;
      return true;
      }
    if(type==MessageType::SnapshotBegin || type==MessageType::SnapshotChunk ||
       type==MessageType::SnapshotEnd || type==MessageType::SnapshotReady) {
      if(peer.stage!=Peer::Connected || !peer.bound || message.generation!=peer.generation ||
         message.player!=PlayerIdentity::Companion) {
        event("invalid_snapshot"); return false;
        }
      if(state.role==Role::Host) {
        if(type!=MessageType::SnapshotReady || peer.transfer!=Peer::AwaitReady || message.revision!=peer.revision) {
          event("invalid_snapshot"); return false;
          }
        peer.transfer = Peer::Ready;
        state.snapshotState = "ready";
        event("snapshot_ready");
        return true;
        }
      if(auto begin = std::get_if<SnapshotBegin>(&message.payload)) {
        if(peer.transfer!=Peer::None) { event("invalid_snapshot"); return false; }
        peer.transfer = Peer::Receiving;
        peer.revision = message.revision;
        peer.total = begin->bytes;
        peer.digest = begin->digest;
        peer.deadline = now+SnapshotTimeoutMs;
        // Allocate only bytes actually received; a declaration alone cannot
        // reserve the maximum-sized snapshot. The completed archive is bounded.
        state.snapshotState = "receiving";
        state.snapshotRevision = peer.revision;
        state.snapshotBytes = peer.total;
        state.snapshotDigest = peer.digest;
        event("snapshot_begin");
        return true;
        }
      if(peer.transfer!=Peer::Receiving || message.revision!=peer.revision) {
        event("invalid_snapshot"); return false;
        }
      if(auto chunk = std::get_if<SnapshotChunk>(&message.payload)) {
        if(chunk->offset!=peer.snapshot.size() || chunk->data.size()>peer.total-peer.snapshot.size()) {
          event("invalid_snapshot_offset"); return false;
          }
        const auto needed = peer.snapshot.size()+chunk->data.size();
        if(needed>peer.snapshot.capacity())
          peer.snapshot.reserve(std::min(size_t(peer.total),std::max(needed,peer.snapshot.capacity()*2)));
        peer.snapshot.insert(peer.snapshot.end(),chunk->data.begin(),chunk->data.end());
        state.snapshotTransferred = peer.snapshot.size();
        return true;
        }
      if(type!=MessageType::SnapshotEnd || peer.snapshot.size()!=peer.total || hashBytes(peer.snapshot)!=peer.digest) {
        event("invalid_snapshot_digest"); return false;
        }
      peer.transfer = Peer::Available;
      state.snapshotState = "received";
      event("snapshot_received");
      return true;
      }
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
    if(peer.tls) peer.tls->beginTick();
    if(peer.transfer!=Peer::None && peer.transfer!=Peer::Ready && now>=peer.deadline) {
      ++state.timeouts;
      event("snapshot_timeout");
      return false;
      }
    if(peer.stage!=Peer::Connected && now-peer.started>=HandshakeTimeoutMs) {
      ++state.timeouts;
      event("timeout");
      return false;
      }
    if(peer.stage==Peer::Connecting) {
      const int result = Net::connected(peer.socket.fd);
      if(result==0) return true;
      if(result<0) { event("connect_error"); return false; }
      peer.stage = peer.tls ? Peer::Authenticating : Peer::Handshake;
      state.state = peer.tls ? "authenticating" : "handshaking";
      }
    if(peer.stage==Peer::Authenticating) {
      const int result=peer.tls->handshake();
      if(result==0) return true;
      if(result<0) { ++state.rejected; event("authentication_failed"); return false; }
      peer.stage=Peer::Handshake;
      if(state.role==Role::Guest) state.state="handshaking";
      }
    // Bound work per frame even if a local peer continuously writes.
    std::array<uint8_t,16384> buffer;
    for(size_t calls=0; calls<4 && peer.stage!=Peer::Closing; ++calls) {
      const auto count = peer.tls ? peer.tls->read(buffer) : Net::receive(peer.socket.fd,buffer);
      if(count<0) {
        if(peer.tls ? peer.tls->blocked() : wouldBlock()) break;
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
    if(peer.transfer==Peer::Sending) {
      for(size_t n=0; n<4 && peer.output.pendingBytes()<128*1024; ++n) {
        if(peer.offset==peer.total) {
          if(!peer.output.enqueue({peer.generation,PlayerIdentity::Companion,0,peer.revision,SnapshotEnd{}})) break;
          peer.transfer = Peer::AwaitReady;
          Bytes{}.swap(peer.snapshot);
          state.snapshotState = "awaiting_ready";
          break;
          }
        const auto count = std::min(uint64_t(SnapshotChunkBytes),peer.total-peer.offset);
        SnapshotChunk chunk{peer.offset,Bytes(peer.snapshot.begin()+std::ptrdiff_t(peer.offset),
                                             peer.snapshot.begin()+std::ptrdiff_t(peer.offset+count))};
        if(!peer.output.enqueue({peer.generation,PlayerIdentity::Companion,0,peer.revision,std::move(chunk)})) break;
        peer.offset += count;
        state.snapshotTransferred = peer.offset;
        }
      }
    for(size_t calls=0; calls<4 && !peer.output.front().empty(); ++calls) {
      auto bytes = peer.output.front();
      const auto count = peer.tls ? peer.tls->write(bytes) : Net::send(peer.socket.fd,bytes);
      if(count<0 && (peer.tls ? peer.tls->blocked() : wouldBlock())) break;
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
  };

Session::Session():impl(std::make_unique<Impl>()) {}
Session::~Session() = default;
const NetworkStatus& Session::status() const { return impl->state; }

bool Session::bindCompanion(uint64_t generation) {
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
  return false;
  }

bool Session::sendSnapshot(uint64_t generation, uint64_t revision, Bytes data) {
  auto& self = *impl;
  if(self.state.role!=Role::Host || data.empty() || data.size()>MaxSnapshotBytes || revision==0) return false;
  for(auto& peer:self.peers) {
    if(peer->stage!=Impl::Peer::Connected || !peer->bound || peer->generation!=generation || peer->transfer!=Impl::Peer::None) continue;
    peer->digest = hashBytes(data);
    if(!peer->output.enqueue({generation,PlayerIdentity::Companion,0,revision,SnapshotBegin{data.size(),peer->digest}})) return false;
    peer->transfer = Impl::Peer::Sending;
    peer->revision = revision;
    peer->total = data.size();
    peer->snapshot = std::move(data);
    // The application calls this immediately after tick(); use that clock.
    peer->deadline = self.now+SnapshotTimeoutMs;
    self.state.snapshotState = "sending";
    self.state.snapshotRevision = revision;
    self.state.snapshotBytes = peer->total;
    self.state.snapshotDigest = peer->digest;
    self.event("snapshot_sending");
    return true;
    }
  return false;
  }

std::optional<Snapshot> Session::takeSnapshot() {
  for(auto& peer:impl->peers) {
    if(peer->transfer!=Impl::Peer::Available) continue;
    peer->transfer = Impl::Peer::Loading;
    impl->state.snapshotState = "loading";
    return Snapshot{peer->generation,peer->revision,std::move(peer->snapshot)};
    }
  return {};
  }

bool Session::snapshotLoaded(uint64_t generation, uint64_t revision) {
  if(impl->state.role!=Role::Guest) return false;
  for(auto& peer:impl->peers) {
    if(peer->transfer!=Impl::Peer::Loading || peer->generation!=generation || peer->revision!=revision) continue;
    if(!peer->output.enqueue({generation,PlayerIdentity::Companion,0,revision,SnapshotReady{}})) return false;
    peer->transfer = Impl::Peer::Ready;
    impl->state.snapshotState = "ready";
    impl->event("snapshot_loaded");
    return true;
    }
  return false;
  }

bool Session::sendInput(Input input) {
  auto& self = *impl;
  if(self.state.role!=Role::Guest || self.state.inputSent==std::numeric_limits<uint64_t>::max()) return false;
  for(auto& peer:self.peers) {
    if(peer->stage!=Impl::Peer::Connected || peer->transfer!=Impl::Peer::Ready) continue;
    const auto sequence = self.state.inputSent+1;
    if(!peer->output.enqueue({peer->generation,PlayerIdentity::Companion,sequence,0,input})) return false;
    self.state.inputSent = sequence;
    return true;
    }
  return false;
  }

void Session::holdInput(bool hold) {
  if(impl->state.role==Role::Host) impl->state.inputHeld = hold;
  }

std::optional<Message> Session::takeInput() {
  if(impl->state.role!=Role::Host || impl->state.inputHeld) return {};
  for(auto& peer:impl->peers) {
    if(peer->inputs.empty()) continue;
    auto message = std::move(peer->inputs.front());
    peer->inputs.pop_front();
    return message;
    }
  return {};
  }

bool Session::inputApplied(uint64_t generation, uint64_t sequence, uint64_t revision) {
  if(impl->state.role!=Role::Host || impl->state.inputHeld) return false;
  for(auto& peer:impl->peers) {
    if(peer->generation!=generation || peer->transfer!=Impl::Peer::Ready ||
       sequence!=peer->appliedInput+1 || sequence>peer->receivedInput) continue;
    if(!peer->output.enqueue({generation,PlayerIdentity::Companion,0,revision,StateAck{sequence}})) return false;
    peer->appliedInput = impl->state.inputApplied = sequence;
    return true;
    }
  return false;
  }

bool Session::sendPoses(uint64_t generation, uint64_t revision, const Poses& poses) {
  auto& self = *impl;
  if(self.state.role!=Role::Host || revision!=std::max(self.state.snapshotRevision,self.state.posesSent)+1 ||
     poses.appliedInput!=self.state.inputApplied || poses.tick<self.state.poseTick) return false;
  for(auto& peer:self.peers) {
    if(peer->generation!=generation || peer->transfer!=Impl::Peer::Ready) continue;
    if(!peer->output.enqueue({generation,PlayerIdentity::Companion,0,revision,poses})) return false;
    self.state.posesSent = revision;
    self.state.poseTick = poses.tick;
    return true;
    }
  return false;
  }

std::optional<Message> Session::takePoses() {
  if(impl->state.role!=Role::Guest) return {};
  for(auto& peer:impl->peers) {
    if(peer->poses.empty()) continue;
    auto message = std::move(peer->poses.front());
    peer->poses.pop_front();
    return message;
    }
  return {};
  }

bool Session::posesApplied(uint64_t generation, uint64_t revision) {
  auto& self = *impl;
  if(self.state.role!=Role::Guest || revision<=self.state.posesApplied || revision>self.state.posesReceived) return false;
  for(auto& peer:self.peers) {
    if(peer->generation!=generation || peer->transfer!=Impl::Peer::Ready) continue;
    if(!peer->output.enqueue({generation,PlayerIdentity::Companion,0,revision,PoseAck{}})) return false;
    self.state.posesApplied = revision;
    return true;
    }
  return false;
  }

void Session::invalidate(const std::string& reason) {
  auto& self = *impl;
  if(self.state.role==Role::Disabled) return;
  for(const auto& peer:self.peers)
    if(peer->stage==Impl::Peer::Connected) ++self.state.disconnected;
  self.peers.clear();
  self.resetSnapshot();
  self.state.inputHeld = false;
  self.state.peers = 0;
  self.state.generation = self.state.boundPlayer = self.state.bindingRevision = 0;
  self.state.state = self.listener ? "listening" : "disconnected";
  self.event(reason);
  }

void Session::start(Endpoint endpoint, const Hello& identity, uint64_t now) {
  impl = std::make_unique<Impl>();
  auto& self = *impl;
  self.state.role = endpoint.role;
  self.state.port = endpoint.port;
  self.state.address = endpoint.address;
  self.identity = identity;
  if(endpoint.role==Role::Disabled) return;
  try {
    Net::initialize();
    if(!endpoint.secret.empty()) self.tlsContext=std::make_shared<TlsContext>(endpoint.secret);
    self.state.encrypted=bool(self.tlsContext);
    if(endpoint.role==Role::Host && !self.tlsContext && (ntohl(address(endpoint.port,endpoint.address).sin_addr.s_addr)>>24)!=127)
      throw std::runtime_error("External listeners require a join secret");
    std::random_device random;
    self.nextGeneration = (uint64_t(random())<<32) | uint64_t(random());
    if(endpoint.role==Role::Guest) {
      sockaddr_in literal{};
      literal.sin_family=AF_INET;
      literal.sin_port=htons(endpoint.port);
      if(inet_pton(AF_INET,endpoint.address.c_str(),&literal.sin_addr)==1) self.connectTo(literal,now);
      else {
        self.resolution=Net::resolve(endpoint.address,endpoint.port);
        self.resolveStarted=now;
        self.state.state="resolving";
        self.event("resolving");
        }
      return;
      }
    const auto addr = address(endpoint.port,endpoint.address);
    const auto fd = ::socket(AF_INET,SOCK_STREAM,0);
    if(fd==Net::Invalid) throw Net::failure("Create co-op socket");
    auto socket = std::make_unique<Socket>(fd);
    nonblocking(fd);
    if(endpoint.role==Role::Host) {
      if(::bind(fd,reinterpret_cast<const sockaddr*>(&addr),sizeof(addr))<0 || ::listen(fd,int(MaxPeers))<0)
        throw Net::failure("Listen on co-op IPv4 socket (address unavailable or port occupied)");
      sockaddr_in bound = {};
      Net::Length length = sizeof(bound);
      if(getsockname(fd,reinterpret_cast<sockaddr*>(&bound),&length)<0)
        throw Net::failure("Read co-op port");
      self.listener = std::move(socket);
      self.state.port = ntohs(bound.sin_port);
      self.state.state = "listening";
      self.event("listening");
      }

    }
  catch(const std::exception& error) {
    self.listener.reset();
    self.peers.clear();
    self.state.state = "failed";
    self.event(std::string("startup_error: ")+error.what());
    }
  }

void Session::tick(uint64_t now) {
  auto& self = *impl;
  self.now = now;
  try {
    if(self.resolution) {
      if(now-self.resolveStarted>=HandshakeTimeoutMs) {
        self.resolution.reset();
        self.state.state="failed";
        ++self.state.timeouts;
        self.event("dns_timeout");
        return;
        }
      if(!self.resolution->ready.load(std::memory_order_acquire)) return;
      auto resolved=std::move(self.resolution);
      if(!resolved->error.empty()) throw std::runtime_error(resolved->error);
      self.connectTo(resolved->address,self.resolveStarted);
      }
    if(self.listener) self.acceptPeers(now);
    for(size_t i=0; i<self.peers.size();) {
      auto& peer = *self.peers[i];
      const bool wasConnected = peer.stage==Impl::Peer::Connected;
      if(self.pump(peer,now)) { ++i; continue; }
      const bool connected = wasConnected || peer.stage==Impl::Peer::Connected;
      if(connected) {
        ++self.state.disconnected;
        self.state.boundPlayer = self.state.bindingRevision = 0;
        self.resetSnapshot();
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
    self.resetSnapshot();
    self.state.state = "failed";
    self.state.peers = 0;
    self.state.generation = self.state.boundPlayer = self.state.bindingRevision = 0;
    self.event(std::string("network_error: ")+error.what());
    }
  }

}
