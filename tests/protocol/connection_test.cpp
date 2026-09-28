#include "network/session.h"
#include "network/fingerprint.h"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <chrono>
#include <fstream>
#include <functional>
#include <iostream>
#include <thread>

using namespace Coop;
namespace {
#define REQUIRE(x) do { if(!(x)) throw std::runtime_error(std::string(__FILE__)+":"+std::to_string(__LINE__)+": " #x); } while(false)

template<class F> void throws(F action) {
  try { action(); } catch(const std::exception&) { return; }
  throw std::runtime_error("Expected an exception");
  }

Hello identity() {
  Hello value;
  value.engine[0] = 12;
  value.gameData[0] = 34;
  return value;
  }

void pump(Session& host, Session& guest, const std::function<bool()>& done, uint64_t now=1) {
  for(int i=0; i<1000; ++i) {
    host.tick(now);
    guest.tick(now);
    if(done()) return;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  throw std::runtime_error("Connection did not reach expected state");
  }

struct Socket {
  int fd;
  Socket():fd(::socket(AF_INET,SOCK_STREAM,0)) { REQUIRE(fd>=0); }
  ~Socket() { ::close(fd); }
  Socket(const Socket&) = delete;
  Socket& operator=(const Socket&) = delete;
  void connect(uint16_t port) {
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    REQUIRE(::connect(fd,reinterpret_cast<sockaddr*>(&addr),sizeof(addr))==0);
    }
  uint16_t listen() {
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    REQUIRE(::bind(fd,reinterpret_cast<sockaddr*>(&addr),sizeof(addr))==0);
    REQUIRE(::listen(fd,1)==0);
    socklen_t size = sizeof(addr);
    REQUIRE(getsockname(fd,reinterpret_cast<sockaddr*>(&addr),&size)==0);
    return ntohs(addr.sin_port);
    }
  };

void endpoints() {
  REQUIRE(hostEndpoint("0").port==0);
  REQUIRE(hostEndpoint("65535").port==65535);
  REQUIRE(joinEndpoint("127.0.0.1:4000").port==4000);
  for(auto text: {"", "-1", "+1", " 4", "1a", "65536", "4294967296"})
    throws([&] { hostEndpoint(text); });
  for(auto text: {"localhost:4", "0.0.0.0:4", "192.168.1.1:4", "[::1]:4", "127.0.0.1:0"})
    throws([&] { joinEndpoint(text); });
  }

void connection() {
  Session host, guest, second;
  host.start({Role::Host,0},identity(),0);
  REQUIRE(host.status().state=="listening");
  REQUIRE(host.status().port!=0);
  guest.start({Role::Guest,host.status().port},identity(),0);
  pump(host,guest,[&] { return host.status().state=="connected" && guest.status().state=="connected"; });
  const auto generation = host.status().generation;
  REQUIRE(generation!=0 && guest.status().generation==generation);
  second.start({Role::Guest,host.status().port},identity(),0);
  pump(host,second,[&] { return second.status().state=="rejected"; });
  REQUIRE(second.status().reason=="host_full");
  REQUIRE(host.status().state=="connected" && host.status().generation==generation);
  guest.start({},identity(),1); // Close only the test-owned guest socket.
  pump(host,guest,[&] { return host.status().state=="listening"; });
  REQUIRE(host.status().disconnected==1);
  guest.start({Role::Guest,host.status().port},identity(),2);
  pump(host,guest,[&] { return guest.status().state=="connected"; },3);
  REQUIRE(guest.status().generation!=generation);
  REQUIRE(host.status().accepted==2);
  }

void rejections() {
  Session host, guest;
  host.start({Role::Host,0},identity(),0);
  REQUIRE(host.status().state=="listening");
  for(bool engine: {true,false}) {
    auto bad = identity();
    if(engine) bad.engine[4] = 1; else bad.gameData[5] = 1;
    guest.start({Role::Guest,host.status().port},bad,0);
    pump(host,guest,[&] { return guest.status().state=="rejected"; });
    REQUIRE(guest.status().reason==(engine ? "engine_mismatch" : "data_mismatch"));
    REQUIRE(host.status().state=="listening");
    }
  {
  Socket bad;
  bad.connect(host.status().port);
  auto bytes = encode({0,0,0,0,identity()});
  bytes[9] = 1; // N03 version 1 cannot participate in the new handshake.
  REQUIRE(::send(bad.fd,bytes.data(),bytes.size(),0)==ssize_t(bytes.size()));
  pump(host,guest,[&] { return host.status().reason=="protocol_mismatch"; });
  }
  REQUIRE(host.status().rejected==3);
  for(const auto& message:std::vector<Message>{{1,1,1,0,Input{}},{1,2,0,1,Welcome{}}}) {
    Socket wrongPhase;
    wrongPhase.connect(host.status().port);
    const auto count = host.status().rejected;
    const auto bytes = encode(message);
    REQUIRE(::send(wrongPhase.fd,bytes.data(),bytes.size(),0)==ssize_t(bytes.size()));
    pump(host,guest,[&] { return host.status().rejected>count; });
    REQUIRE(host.status().reason=="invalid_handshake");
    REQUIRE(host.status().state=="listening");
    }
  guest.start({Role::Guest,host.status().port},identity(),2);
  pump(host,guest,[&] { return guest.status().state=="connected"; },3);
  // Guest must verify the host's fingerprints, even if that host claims success.
  for(bool engine: {true,false}) {
    Socket dishonest;
    const auto port = dishonest.listen();
    Session client;
    client.start({Role::Guest,port},identity(),0);
    client.tick(1);
    Socket accepted;
    ::close(accepted.fd);
    accepted.fd = ::accept(dishonest.fd,nullptr,nullptr);
    REQUIRE(accepted.fd>=0);
    auto bad = identity();
    if(engine) bad.engine[1] ^= 1; else bad.gameData[1] ^= 1;
    auto reply = encode({1,0,0,0,Accepted{bad}});
    REQUIRE(::send(accepted.fd,reply.data(),reply.size(),0)==ssize_t(reply.size()));
    pump(host,client,[&] { return client.status().state=="rejected"; },2);
    REQUIRE(client.status().reason==(engine ? "engine_mismatch" : "data_mismatch"));
    }
  }

void binding() {
  Session host, guest, second;
  host.start({Role::Host,0},identity(),0);
  REQUIRE(host.status().state=="listening");
  const auto port = host.status().port;
  uint64_t previous = 0;
  for(uint64_t cycle=1; cycle<=3; ++cycle) {
    guest.start({Role::Guest,port},identity(),0);
    pump(host,guest,[&] { return guest.status().state=="connected"; });
    const auto generation = host.status().generation;
    REQUIRE(generation!=0 && generation!=previous);
    REQUIRE(host.status().boundPlayer==0 && guest.status().boundPlayer==0);
    REQUIRE(!guest.bindCompanion(generation)); // No guest API can assign authority.
    REQUIRE(!host.bindCompanion(previous));
    REQUIRE(!host.bindCompanion(generation+1));
    REQUIRE(host.bindCompanion(generation));
    REQUIRE(host.bindCompanion(generation)); // Must not enqueue another Welcome.
    pump(host,guest,[&] { return guest.status().boundPlayer==2; });
    REQUIRE(host.status().boundPlayer==2 && host.status().bindings==cycle);
    REQUIRE(guest.status().bindings==1 && guest.status().bindingRevision==1);
    REQUIRE(guest.status().generation==generation);
    second.start({Role::Guest,port},identity(),0);
    pump(host,second,[&] { guest.tick(1); return second.status().state=="rejected"; });
    REQUIRE(second.status().reason=="host_full");
    REQUIRE(guest.status().state=="connected" && guest.status().bindings==1);
    REQUIRE(host.status().boundPlayer==2 && host.status().generation==generation);
    if(cycle==2) {
      host.invalidate("world_replaced");
      REQUIRE(host.status().state=="listening" && host.status().port==port);
      REQUIRE(host.status().peers==0 && host.status().boundPlayer==0);
      pump(host,guest,[&] { return guest.status().state=="disconnected"; });
      REQUIRE(guest.status().boundPlayer==0 && guest.status().bindingRevision==0);
      REQUIRE(guest.status().generation==0);
      }
    else {
      guest.start({},identity(),1);
      pump(host,guest,[&] { return host.status().state=="listening"; });
      }
    REQUIRE(host.status().boundPlayer==0 && host.status().bindingRevision==0);
    REQUIRE(host.status().generation==0 && !host.bindCompanion(generation));
    previous = generation;
    }
  }

void sendMessage(Socket& socket, const Message& message) {
  const auto bytes = encode(message);
  REQUIRE(::send(socket.fd,bytes.data(),bytes.size(),0)==ssize_t(bytes.size()));
  }

void bindingAuthority() {
  Session host, unused;
  host.start({Role::Host,0},identity(),0);
  REQUIRE(host.status().state=="listening");
  // A compatible guest still has no authority to grant itself any identity.
  for(bool bindFirst: {false,true}) {
    for(uint64_t player: {1u,2u,3u}) {
      Socket guest;
      guest.connect(host.status().port);
      sendMessage(guest,{0,0,0,0,identity()});
      pump(host,unused,[&] { return host.status().state=="connected"; });
      const auto generation = host.status().generation;
      if(bindFirst) REQUIRE(host.bindCompanion(generation));
      sendMessage(guest,{generation,player,0,1,Welcome{}});
      pump(host,unused,[&] { return host.status().state=="listening"; });
      REQUIRE(host.status().reason=="authority_violation");
      REQUIRE(host.status().boundPlayer==0);
      }
    }
  // Inputs remain disabled for *every* player, including the assigned companion.
  for(uint64_t player: {1u,2u,3u}) {
    Socket guest;
    guest.connect(host.status().port);
    sendMessage(guest,{0,0,0,0,identity()});
    pump(host,unused,[&] { return host.status().state=="connected"; });
    const auto generation = host.status().generation;
    REQUIRE(host.bindCompanion(generation));
    sendMessage(guest,{generation,player,1,0,Input{Forward,0,0}});
    pump(host,unused,[&] { return host.status().state=="listening"; });
    REQUIRE(host.status().reason==(player==2 ? "unexpected_message" : "authority_violation"));
    }
  // Even a compatible server may not assign host/unknown IDs, change generation,
  // skip Accepted, or rebind an already bound client.
  for(int fault=0; fault<6; ++fault) {
    Socket server;
    Session guest;
    guest.start({Role::Guest,server.listen()},identity(),0);
    guest.tick(1);
    Socket accepted;
    ::close(accepted.fd);
    accepted.fd = ::accept(server.fd,nullptr,nullptr);
    REQUIRE(accepted.fd>=0);
    if(fault!=4) sendMessage(accepted,{12,0,0,0,Accepted{identity()}});
    if(fault==5) sendMessage(accepted,{12,2,0,1,Welcome{}});
    const uint64_t player = fault==0 ? 1 : fault==1 ? 3 : 2;
    sendMessage(accepted,{fault==2 ? 13u : 12u,player,0,fault==3 ? 2u : 1u,Welcome{}});
    pump(host,guest,[&] { return guest.status().state=="failed" || guest.status().state=="disconnected"; });
    REQUIRE(guest.status().reason==(fault==4 ? "invalid_handshake" : "invalid_binding"));
    REQUIRE(guest.status().boundPlayer==0 && guest.status().bindingRevision==0);
    }
  }

void timeout() {
  Session host, guest;
  host.start({Role::Host,0},identity(),0);
  REQUIRE(host.status().state=="listening");
  Socket silent;
  silent.connect(host.status().port);
  pump(host,guest,[&] { return host.status().peers==1; });
  host.tick(1+Session::HandshakeTimeoutMs);
  REQUIRE(host.status().state=="listening" && host.status().timeouts==1);
  REQUIRE(host.status().peers==0 && host.status().reason=="timeout");
  guest.start({Role::Guest,host.status().port},identity(),6000);
  pump(host,guest,[&] { return guest.status().state=="connected"; },6001);
  // Guest deadline expires even if connect/handshake is never serviced.
  Session waiting;
  waiting.start({Role::Guest,host.status().port},identity(),7000);
  waiting.tick(7000+Session::HandshakeTimeoutMs);
  REQUIRE(waiting.status().state=="failed" && waiting.status().reason=="timeout");
  // Silent peers cannot grow the pending socket pool indefinitely.
  Session bounded;
  bounded.start({Role::Host,0},identity(),0);
  REQUIRE(bounded.status().state=="listening");
  std::vector<std::unique_ptr<Socket>> idle;
  for(size_t i=0; i<Session::MaxPeers+1; ++i) {
    auto peer = std::make_unique<Socket>();
    peer->connect(bounded.status().port);
    idle.push_back(std::move(peer));
    bounded.tick(1);
    }
  // A completed client connect need not be accept-ready in that same host tick.
  // Wait for the rejection with a fixed logical clock, without expiring peers.
  Session unused;
  pump(bounded,unused,[&] { return bounded.status().reason=="pending_limit"; });
  REQUIRE(bounded.status().peers==Session::MaxPeers);
  REQUIRE(bounded.status().reason=="pending_limit");
  bounded.tick(1+Session::HandshakeTimeoutMs);
  REQUIRE(bounded.status().peers==0);
  REQUIRE(bounded.status().timeouts==Session::MaxPeers);
  }

void snapshotRoundtrip() {
  Session host, guest;
  host.start({Role::Host,0},identity(),0);
  guest.start({Role::Guest,host.status().port},identity(),0);
  pump(host,guest,[&] { return host.status().state=="connected"; });
  const auto generation = host.status().generation;
  REQUIRE(host.bindCompanion(generation));
  Bytes source(1024*1024+17);
  for(size_t i=0; i<source.size(); ++i) source[i] = uint8_t(i*37);
  REQUIRE(!host.sendSnapshot(generation,0,source));
  REQUIRE(!guest.sendSnapshot(generation,1,source));
  REQUIRE(host.sendSnapshot(generation,9,source));
  REQUIRE(!host.sendSnapshot(generation,9,source));
  pump(host,guest,[&] { return guest.status().snapshotState=="received"; });
  REQUIRE(host.status().snapshotState=="awaiting_ready");
  auto snapshot = guest.takeSnapshot();
  REQUIRE(snapshot && snapshot->generation==generation && snapshot->revision==9 && snapshot->data==source);
  REQUIRE(!guest.takeSnapshot());
  REQUIRE(guest.status().snapshotDigest==hashBytes(source));
  REQUIRE(!guest.snapshotLoaded(generation+1,9));
  REQUIRE(!guest.snapshotLoaded(generation,8));
  REQUIRE(guest.snapshotLoaded(generation,9));
  REQUIRE(!guest.snapshotLoaded(generation,9));
  pump(host,guest,[&] { return host.status().snapshotState=="ready"; });
  REQUIRE(guest.status().snapshotState=="ready");
  }

void snapshotFailures() {
  // Real stream interruption, with a nonempty partial archive buffered.
  Session host, guest;
  host.start({Role::Host,0},identity(),0);
  guest.start({Role::Guest,host.status().port},identity(),0);
  pump(host,guest,[&] { return host.status().state=="connected"; });
  REQUIRE(host.bindCompanion(host.status().generation));
  REQUIRE(host.sendSnapshot(host.status().generation,1,Bytes(1024*1024,42)));
  pump(host,guest,[&] { return guest.status().snapshotTransferred>0; });
  REQUIRE(guest.status().snapshotTransferred<guest.status().snapshotBytes);
  host.invalidate("test_interruption");
  pump(host,guest,[&] { return guest.status().state=="disconnected"; });
  REQUIRE(!guest.takeSnapshot() && guest.status().snapshotBytes==0);

  const Bytes data{4,5,6};
  for(int fault=0; fault<9; ++fault) {
    Socket server;
    guest.start({Role::Guest,server.listen()},identity(),0);
    guest.tick(1);
    Socket peer;
    ::close(peer.fd);
    peer.fd = ::accept(server.fd,nullptr,nullptr);
    REQUIRE(peer.fd>=0);
    sendMessage(peer,{12,0,0,0,Accepted{identity()}});
    sendMessage(peer,{12,2,0,1,Welcome{}});
    const Message begin{12,2,0,1,SnapshotBegin{data.size(),hashBytes(data)}};
    sendMessage(peer,begin);
    if(fault==0) sendMessage(peer,begin); // Duplicate begin.
    if(fault==1) sendMessage(peer,{12,2,0,1,SnapshotChunk{1,data}});
    if(fault==2) sendMessage(peer,{13,2,0,1,SnapshotChunk{0,data}});
    if(fault==3) sendMessage(peer,{12,1,0,1,SnapshotChunk{0,data}});
    if(fault==4) sendMessage(peer,{12,2,0,2,SnapshotChunk{0,data}});
    if(fault==5) sendMessage(peer,{12,2,0,1,SnapshotEnd{}});
    if(fault==6) {
      sendMessage(peer,{12,2,0,1,SnapshotChunk{0,{4,5,7}}});
      sendMessage(peer,{12,2,0,1,SnapshotEnd{}});
      }
    if(fault==7) sendMessage(peer,{12,2,0,1,SnapshotReady{}});
    if(fault==8) {
      pump(host,guest,[&] { return guest.status().snapshotState=="receiving"; });
      guest.tick(Session::SnapshotTimeoutMs+1);
      }
    pump(host,guest,[&] { return guest.status().state=="disconnected"; });
    REQUIRE(!guest.takeSnapshot() && guest.status().boundPlayer==0);
    }
  // Completed bytes awaiting a loader must also be fenced by disconnect.
  host.start({Role::Host,0},identity(),0);
  guest.start({Role::Guest,host.status().port},identity(),0);
  pump(host,guest,[&] { return host.status().state=="connected"; });
  const auto generation = host.status().generation;
  REQUIRE(host.bindCompanion(generation));
  REQUIRE(host.sendSnapshot(generation,1,data));
  pump(host,guest,[&] { return guest.status().snapshotState=="received"; });
  REQUIRE(guest.takeSnapshot());
  host.invalidate("test_loading_interruption");
  pump(host,guest,[&] { return guest.status().state=="disconnected"; });
  REQUIRE(!guest.snapshotLoaded(generation,1));
  }

void inputRoundtrip() {
  Session host, guest;
  host.start({Role::Host,0},identity(),0);
  guest.start({Role::Guest,host.status().port},identity(),0);
  REQUIRE(!guest.sendInput({Forward,0,0}));
  pump(host,guest,[&] { return host.status().state=="connected"; });
  const auto generation = host.status().generation;
  REQUIRE(host.bindCompanion(generation));
  REQUIRE(!guest.sendInput({Forward,0,0}));
  REQUIRE(host.sendSnapshot(generation,1,Bytes{1,2,3}));
  pump(host,guest,[&] { return guest.status().snapshotState=="received"; });
  REQUIRE(guest.takeSnapshot());
  REQUIRE(guest.snapshotLoaded(generation,1));
  pump(host,guest,[&] { return host.status().snapshotState=="ready"; });
  for(auto input:{Input{Forward,0,0},Input{TurnRight,10,-20},Input{0,0,0}})
    REQUIRE(guest.sendInput(input));
  pump(host,guest,[&] { return host.status().inputReceived==3; });
  REQUIRE(host.status().inputApplied==0 && guest.status().inputAcknowledged==0);
  REQUIRE(!host.inputApplied(generation,2,1));
  for(uint64_t sequence=1; sequence<=3; ++sequence) {
    auto message = host.takeInput();
    REQUIRE(message && message->generation==generation && message->player==2 && message->sequence==sequence);
    REQUIRE(host.inputApplied(generation,sequence,1));
    REQUIRE(!host.inputApplied(generation,sequence,1));
    }
  REQUIRE(!host.takeInput());
  pump(host,guest,[&] { return guest.status().inputAcknowledged==3; });
  REQUIRE(guest.status().acknowledgedRevision==1);
  host.invalidate("test_input_reset");
  REQUIRE(!host.takeInput() && host.status().inputApplied==0);
  }

void inputDisconnect() {
  Session host, guest;
  host.start({Role::Host,0},identity(),0);
  guest.start({Role::Guest,host.status().port},identity(),0);
  pump(host,guest,[&] { return host.status().state=="connected"; });
  const auto generation = host.status().generation;
  REQUIRE(host.bindCompanion(generation) && host.sendSnapshot(generation,1,Bytes{1}));
  pump(host,guest,[&] { return guest.status().snapshotState=="received"; });
  REQUIRE(guest.takeSnapshot() && guest.snapshotLoaded(generation,1));
  pump(host,guest,[&] { return host.status().snapshotState=="ready"; });
  REQUIRE(guest.sendInput({Forward,20,-10}));
  pump(host,guest,[&] { return host.status().inputReceived==1; });
  REQUIRE(host.takeInput() && host.inputApplied(generation,1,2));
  // Leave movement/turn impulses queued, without a key release or application.
  REQUIRE(guest.sendInput({Forward|TurnRight,100,100}));
  pump(host,guest,[&] { return host.status().inputReceived==2; });
  guest.start({},identity(),1);
  pump(host,guest,[&] { return host.status().state=="listening"; });
  const auto& state = host.status();
  REQUIRE(state.disconnected==1 && state.generation==0 && state.boundPlayer==0);
  REQUIRE(state.snapshotState=="none" && state.inputReceived==0 && state.inputApplied==0);
  REQUIRE(!host.takeInput() && !host.inputApplied(generation,2,2));
  REQUIRE(state.port!=0); // The host listener survives the lost controller.
  }

void saveInputHold() {
  Session host, guest;
  host.start({Role::Host,0},identity(),0);
  guest.start({Role::Guest,host.status().port},identity(),0);
  pump(host,guest,[&] { return host.status().state=="connected"; });
  const auto generation = host.status().generation;
  REQUIRE(host.bindCompanion(generation) && host.sendSnapshot(generation,1,Bytes{1}));
  pump(host,guest,[&] { return guest.status().snapshotState=="received"; });
  REQUIRE(guest.takeSnapshot() && guest.snapshotLoaded(generation,1));
  pump(host,guest,[&] { return host.status().snapshotState=="ready"; });
  REQUIRE(guest.sendInput({Forward,0,0}));
  pump(host,guest,[&] { return host.status().inputReceived==1; });
  REQUIRE(host.takeInput() && host.inputApplied(generation,1,2));
  host.holdInput(true);
  REQUIRE(guest.sendInput({Forward,50,0}) && guest.sendInput({0,0,0}));
  pump(host,guest,[&] { return host.status().inputReceived==3; });
  REQUIRE(host.status().inputHeld && host.status().inputApplied==1);
  REQUIRE(!host.takeInput() && !host.inputApplied(generation,2,3));
  // Both successful and failed writers release the same FIFO after completion.
  host.holdInput(false);
  for(uint64_t sequence=2; sequence<=3; ++sequence) {
    const auto input = host.takeInput();
    REQUIRE(input && input->sequence==sequence && input->generation==generation);
    REQUIRE(host.inputApplied(generation,sequence,3));
    REQUIRE(!host.inputApplied(generation,sequence,3));
    }
  REQUIRE(!host.takeInput());
  pump(host,guest,[&] { return guest.status().inputAcknowledged==3; });
  host.holdInput(true);
  REQUIRE(guest.sendInput({Forward,100,0}));
  pump(host,guest,[&] { return host.status().inputReceived==4; });
  guest.start({},identity(),1);
  pump(host,guest,[&] { return host.status().state=="listening"; });
  host.holdInput(false);
  REQUIRE(!host.takeInput() && host.status().inputApplied==0 && host.status().generation==0);
  }

void inputValidation() {
  Session host, idle;
  host.start({Role::Host,0},identity(),0);
  auto ready = [&](Socket& peer) {
    peer.connect(host.status().port);
    sendMessage(peer,{0,0,0,0,identity()});
    pump(host,idle,[&] { return host.status().state=="connected"; });
    const auto gen = host.status().generation;
    REQUIRE(host.bindCompanion(gen));
    REQUIRE(host.sendSnapshot(gen,1,Bytes{1}));
    pump(host,idle,[&] { return host.status().snapshotState=="awaiting_ready"; });
    sendMessage(peer,{gen,2,0,1,SnapshotReady{}});
    pump(host,idle,[&] { return host.status().snapshotState=="ready"; });
    return gen;
    };
  Socket peer;
  const auto gen = ready(peer);
  sendMessage(peer,{gen,2,1,0,Input{0,25,0}});
  pump(host,idle,[&] { return host.status().inputReceived==1; });
  REQUIRE(host.takeInput());
  REQUIRE(host.inputApplied(gen,1,1));
  sendMessage(peer,{gen,2,1,0,Input{Forward,1000,0}});
  sendMessage(peer,{gen+1,2,2,0,Input{Forward,1000,0}});
  pump(host,idle,[&] { return host.status().inputDuplicates==1 && host.status().inputStale==1; });
  REQUIRE(!host.takeInput() && host.status().inputApplied==1);
  sendMessage(peer,{gen,2,2,0,Input{0,0,0}});
  pump(host,idle,[&] { return host.status().inputReceived==2; });
  REQUIRE(host.takeInput());
  REQUIRE(host.inputApplied(gen,2,1));
  sendMessage(peer,{gen,2,4,0,Input{Forward,0,0}});
  pump(host,idle,[&] { return host.status().state=="listening"; });
  REQUIRE(host.status().reason=="input_sequence_gap");
  for(int fault=0; fault<3; ++fault) {
    Socket invalid;
    const auto generation = ready(invalid);
    if(fault==0) sendMessage(invalid,{generation,1,1,0,Input{Forward,0,0}});
    if(fault==1) sendMessage(invalid,{generation,2,1,0,Input{Jump,0,0}});
    if(fault==2)
      for(uint64_t i=1; i<=65; ++i) sendMessage(invalid,{generation,2,i,0,Input{Forward,0,0}});
    pump(host,idle,[&] { return host.status().state=="listening"; });
    REQUIRE(host.status().reason==(fault==0 ? "authority_violation" : fault==1 ? "unsupported_input" : "input_queue_full"));
    REQUIRE(!host.takeInput());
    }
  // A host cannot acknowledge input the guest never sent.
  Socket server;
  Session guest;
  guest.start({Role::Guest,server.listen()},identity(),0);
  guest.tick(1);
  Socket accepted;
  ::close(accepted.fd);
  accepted.fd = ::accept(server.fd,nullptr,nullptr);
  REQUIRE(accepted.fd>=0);
  sendMessage(accepted,{12,0,0,0,Accepted{identity()}});
  sendMessage(accepted,{12,2,0,1,Welcome{}});
  sendMessage(accepted,{12,2,0,1,SnapshotBegin{1,hashBytes(Bytes{7})}});
  sendMessage(accepted,{12,2,0,1,SnapshotChunk{0,Bytes{7}}});
  sendMessage(accepted,{12,2,0,1,SnapshotEnd{}});
  pump(idle,guest,[&] { return guest.status().snapshotState=="received"; });
  REQUIRE(guest.takeSnapshot() && guest.snapshotLoaded(12,1));
  sendMessage(accepted,{12,2,0,1,StateAck{1}});
  pump(idle,guest,[&] { return guest.status().state=="disconnected"; });
  REQUIRE(guest.status().reason=="invalid_input_ack");
  }

Poses posesFixture(uint64_t tick=100, uint64_t input=0) {
  Poses poses;
  poses.world = 99; poses.tick = tick; poses.appliedInput = input;
  for(size_t i=0; i<2; ++i) {
    poses.actors[i].player = i+1; poses.actors[i].object = i+11;
    poses.actors[i].position = {float(i)*100.f,10.f,-5.f};
    poses.actors[i].layers.push_back({"S_RUN",50,0,3,0});
    }
  return poses;
  }

void posesRoundtrip() {
  Session host, guest;
  host.start({Role::Host,0},identity(),0);
  guest.start({Role::Guest,host.status().port},identity(),0);
  pump(host,guest,[&] { return host.status().state=="connected"; });
  const auto gen = host.status().generation;
  REQUIRE(!host.sendPoses(gen,2,posesFixture()));
  REQUIRE(host.bindCompanion(gen) && host.sendSnapshot(gen,1,Bytes{7}));
  pump(host,guest,[&] { return guest.status().snapshotState=="received"; });
  REQUIRE(guest.takeSnapshot() && guest.snapshotLoaded(gen,1));
  pump(host,guest,[&] { return host.status().snapshotState=="ready"; });
  REQUIRE(host.sendPoses(gen,2,posesFixture()));
  pump(host,guest,[&] { return guest.status().posesReceived==2; });
  const auto message = guest.takePoses();
  REQUIRE(message && message->revision==2 && std::get<Poses>(message->payload)==posesFixture());
  REQUIRE(host.status().posesAcknowledged==0 && !guest.takePoses());
  REQUIRE(!guest.posesApplied(gen+1,2) && !guest.posesApplied(gen,3));
  REQUIRE(guest.posesApplied(gen,2));
  REQUIRE(!guest.posesApplied(gen,2));
  pump(host,guest,[&] { return host.status().posesAcknowledged==2; });
  REQUIRE(guest.sendInput({Forward,0,0}));
  pump(host,guest,[&] { return host.status().inputReceived==1; });
  REQUIRE(host.takeInput() && host.inputApplied(gen,1,3));
  REQUIRE(!host.sendPoses(gen,3,posesFixture(99,1)));
  REQUIRE(host.sendPoses(gen,3,posesFixture(130,1)));
  pump(host,guest,[&] { return guest.status().posesReceived==3; });
  REQUIRE(guest.status().inputAcknowledged==1 && guest.status().acknowledgedRevision==3);
  REQUIRE(guest.takePoses() && guest.posesApplied(gen,3));
  pump(host,guest,[&] { return host.status().posesAcknowledged==3; });
  }

void rejoinQueues() {
  Session host, guest;
  host.start({Role::Host,0},identity(),0);
  guest.start({Role::Guest,host.status().port},identity(),0);
  pump(host,guest,[&] { return host.status().state=="connected"; });
  const auto old = host.status().generation;
  REQUIRE(host.bindCompanion(old) && host.sendSnapshot(old,1,Bytes{7}));
  pump(host,guest,[&] { return guest.status().snapshotState=="received"; });
  REQUIRE(guest.takeSnapshot() && guest.snapshotLoaded(old,1));
  pump(host,guest,[&] { return host.status().snapshotState=="ready"; });
  REQUIRE(guest.sendInput({Forward,10,0}));
  pump(host,guest,[&] { return host.status().inputReceived==1; });
  // Leave both host input and guest presentation unapplied in their queues.
  REQUIRE(host.sendPoses(old,2,posesFixture()));
  pump(host,guest,[&] { return guest.status().posesReceived==2; });
  REQUIRE(guest.sendInput({TurnRight,100,0})); // Also leave unsent guest bytes.
  guest.start({},identity(),2);
  pump(host,guest,[&] { return host.status().state=="listening"; },3);
  guest.start({Role::Guest,host.status().port},identity(),2);
  pump(host,guest,[&] { return host.status().accepted==2 && guest.status().state=="connected"; },3);
  const auto generation = host.status().generation;
  REQUIRE(generation!=old && generation!=0 && guest.status().generation==generation);
  REQUIRE(!host.takeInput() && !guest.takePoses());
  REQUIRE(host.status().inputReceived==0 && guest.status().inputSent==0 && guest.status().posesReceived==0);
  REQUIRE(!guest.snapshotLoaded(old,1) && !guest.posesApplied(old,2));
  REQUIRE(!host.inputApplied(old,1,2) && !host.sendPoses(old,2,posesFixture()));
  REQUIRE(host.bindCompanion(generation) && host.sendSnapshot(generation,1,Bytes{8,9}));
  pump(host,guest,[&] { return guest.status().snapshotState=="received"; },4);
  REQUIRE(!guest.sendInput({Forward,0,0}));
  const auto snapshot = guest.takeSnapshot();
  REQUIRE(snapshot && snapshot->generation==generation && snapshot->data==Bytes({8,9}));
  REQUIRE(guest.snapshotLoaded(generation,1));
  pump(host,guest,[&] { return host.status().snapshotState=="ready"; },5);
  REQUIRE(guest.sendInput({0,0,0}));
  pump(host,guest,[&] { return host.status().inputReceived==1; },6);
  const auto input = host.takeInput();
  REQUIRE(input && input->generation==generation && input->sequence==1 && std::get<Input>(input->payload).buttons==0);
  REQUIRE(host.inputApplied(generation,1,2));
  REQUIRE(host.sendPoses(generation,2,posesFixture(100,1)));
  pump(host,guest,[&] { return guest.status().posesReceived==2; },7);
  const auto pose = guest.takePoses();
  REQUIRE(pose && pose->generation==generation && guest.status().inputAcknowledged==1);
  REQUIRE(guest.posesApplied(generation,2));
  pump(host,guest,[&] { return host.status().posesAcknowledged==2; },8);
  }

void rejoinStale() {
  Session idle, guest;
  // Reuse one guest Session across old and fresh host generations. Inject old
  // poses/acks after the fresh snapshot; no old application work may escape.
  for(bool stalePose:{true,false}) {
    Socket server;
    const auto port = server.listen();
    for(uint64_t generation:{uint64_t(12),uint64_t(13)}) {
      guest.start({Role::Guest,port},identity(),0);
      guest.tick(1);
      Socket peer;
      ::close(peer.fd);
      peer.fd = ::accept(server.fd,nullptr,nullptr);
      REQUIRE(peer.fd>=0);
      sendMessage(peer,{generation,0,0,0,Accepted{identity()}});
      sendMessage(peer,{generation,2,0,1,Welcome{}});
      sendMessage(peer,{generation,2,0,1,SnapshotBegin{1,hashBytes(Bytes{7})}});
      sendMessage(peer,{generation,2,0,1,SnapshotChunk{0,Bytes{7}}});
      sendMessage(peer,{generation,2,0,1,SnapshotEnd{}});
      pump(idle,guest,[&] { return guest.status().snapshotState=="received"; });
      REQUIRE(guest.takeSnapshot() && guest.snapshotLoaded(generation,1));
      if(generation==12) {
        sendMessage(peer,{12,2,0,2,posesFixture()});
        pump(idle,guest,[&] { return guest.status().posesReceived==2; });
        }
      else {
        REQUIRE(!guest.takePoses());
        if(stalePose) sendMessage(peer,{12,2,0,2,posesFixture()});
        else          sendMessage(peer,{12,2,0,2,StateAck{0}});
        pump(idle,guest,[&] { return guest.status().state=="disconnected"; });
        REQUIRE(!guest.takePoses() && guest.status().posesApplied==0 && guest.status().inputAcknowledged==0);
        REQUIRE(guest.status().reason==(stalePose ? "invalid_pose_session" : "invalid_input_ack"));
        }
      }
    }
  }

void posesValidation() {
  Session idle, guest;
  for(int fault=0; fault<7; ++fault) {
    Socket server;
    guest.start({Role::Guest,server.listen()},identity(),0);
    guest.tick(1);
    Socket peer;
    ::close(peer.fd);
    peer.fd = ::accept(server.fd,nullptr,nullptr);
    REQUIRE(peer.fd>=0);
    sendMessage(peer,{12,0,0,0,Accepted{identity()}});
    sendMessage(peer,{12,2,0,1,Welcome{}});
    sendMessage(peer,{12,2,0,1,SnapshotBegin{1,hashBytes(Bytes{7})}});
    sendMessage(peer,{12,2,0,1,SnapshotChunk{0,Bytes{7}}});
    sendMessage(peer,{12,2,0,1,SnapshotEnd{}});
    pump(idle,guest,[&] { return guest.status().snapshotState=="received"; });
    REQUIRE(guest.takeSnapshot() && guest.snapshotLoaded(12,1));
    if(fault==0) sendMessage(peer,{13,2,0,2,posesFixture()});
    if(fault==1) sendMessage(peer,{12,1,0,2,posesFixture()});
    if(fault==2) sendMessage(peer,{12,2,0,3,posesFixture()});
    if(fault==3) sendMessage(peer,{12,2,0,2,posesFixture(100,1)});
    if(fault==4 || fault==5) {
      sendMessage(peer,{12,2,0,2,posesFixture()});
      sendMessage(peer,{12,2,0,fault==4 ? 3u : 2u,posesFixture(fault==4 ? 99 : 100)});
      }
    if(fault==6) {
      // Keep the reader ticking but deliberately never let the application
      // take frames: its queue must fail closed at 65, not grow indefinitely.
      for(uint64_t i=2; i<=66; ++i) {
        sendMessage(peer,{12,2,0,i,posesFixture()});
        if(i<66) pump(idle,guest,[&] { return guest.status().posesReceived==i; });
        }
      }
    pump(idle,guest,[&] { return guest.status().state=="disconnected"; });
    REQUIRE(!guest.takePoses() && guest.status().posesApplied==0);
    }
  Session host;
  host.start({Role::Host,0},identity(),0);
  Socket peer;
  peer.connect(host.status().port);
  sendMessage(peer,{0,0,0,0,identity()});
  pump(host,idle,[&] { return host.status().state=="connected"; });
  const auto gen = host.status().generation;
  REQUIRE(host.bindCompanion(gen) && host.sendSnapshot(gen,1,Bytes{7}));
  pump(host,idle,[&] { return host.status().snapshotState=="awaiting_ready"; });
  sendMessage(peer,{gen,2,0,1,SnapshotReady{}});
  pump(host,idle,[&] { return host.status().snapshotState=="ready"; });
  sendMessage(peer,{gen,2,0,2,PoseAck{}}); // Nothing at revision 2 has been sent.
  pump(host,idle,[&] { return host.status().state=="listening"; });
  REQUIRE(host.status().reason=="invalid_pose_ack");
  }

struct Fixture {
  std::filesystem::path root = std::filesystem::temp_directory_path()/
    ("gothic-fingerprint-"+std::to_string(getpid()));
  Fixture() {
    std::filesystem::create_directories(root/"Data");
    std::filesystem::create_directories(root/"_work");
    }
  ~Fixture() { std::filesystem::remove_all(root); }
  void write(const std::string& name,const std::string& contents) {
    std::ofstream file(root/name,std::ios::binary);
    file << contents;
    REQUIRE(bool(file));
    }
  };

void fingerprints() {
  Fixture fixture;
  fixture.write("engine","abc");
  fixture.write("Data/a.vdf","archive A");
  fixture.write("Data/b.vdf","archive B");
  fixture.write("_work/game.dat","compiled script");
  fixture.write("mod.ini","selected mod");
  const std::atomic_bool cancel = false;
  const auto& root = fixture.root;
  REQUIRE(hexFingerprint(hashFile(root/"engine",cancel))==
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  FingerprintInputs inputs{root/"engine",root/"Data",root/"_work",root/"mod.ini",
                           {root/"Data/a.vdf",root/"Data/b.vdf"},{"game=2","language=0"}};
  const auto original = fingerprint(inputs,cancel);
  REQUIRE(original==fingerprint(inputs,cancel));
  // Installation directory and filesystem enumeration order are not identity.
  std::filesystem::copy(root,root.string()+"-copy",std::filesystem::copy_options::recursive);
  const std::filesystem::path copied = root.string()+"-copy";
  FingerprintInputs relocated{copied/"engine",copied/"Data",copied/"_work",copied/"mod.ini",
                              {copied/"Data/a.vdf",copied/"Data/b.vdf"},inputs.settings};
  const auto moved = fingerprint(relocated,cancel);
  std::filesystem::remove_all(copied);
  REQUIRE(moved==original);
  const auto time = std::filesystem::last_write_time(root/"Data/a.vdf");
  fixture.write("Data/a.vdf","archive C"); // Same byte count and mtime; content must still matter.
  std::filesystem::last_write_time(root/"Data/a.vdf",time);
  REQUIRE(fingerprint(inputs,cancel).gameData!=original.gameData);
  fixture.write("Data/a.vdf","archive A");
  REQUIRE(fingerprint(inputs,cancel)==original);
  std::swap(inputs.mountedArchives[0],inputs.mountedArchives[1]);
  REQUIRE(fingerprint(inputs,cancel).gameData!=original.gameData);
  std::swap(inputs.mountedArchives[0],inputs.mountedArchives[1]);
  inputs.settings[1] = "language=1";
  REQUIRE(fingerprint(inputs,cancel).gameData!=original.gameData);
  inputs.settings[1] = "language=0";
  fixture.write("mod.ini","different mod");
  REQUIRE(fingerprint(inputs,cancel).gameData!=original.gameData);
  fixture.write("mod.ini","selected mod");
  fixture.write("_work/game.dat","modified script");
  REQUIRE(fingerprint(inputs,cancel).gameData!=original.gameData);
  fixture.write("_work/game.dat","compiled script");
  fixture.write("engine","abd");
  const auto changed = fingerprint(inputs,cancel);
  REQUIRE(changed.engine!=original.engine && changed.gameData==original.gameData);
  throws([&] { const std::atomic_bool stop=true; fingerprint(inputs,stop); });
  std::filesystem::create_symlink(root/"engine",root/"Data/symlink");
  throws([&] { fingerprint(inputs,cancel); });
  }
}

int main(int argc, char** argv) {
  try {
    REQUIRE(argc==2);
    const std::string name = argv[1];
    if(name=="endpoints") endpoints();
    else if(name=="connection") connection();
    else if(name=="rejections") rejections();
    else if(name=="timeout") timeout();
    else if(name=="fingerprints") fingerprints();
    else if(name=="binding") binding();
    else if(name=="binding-authority") bindingAuthority();
    else if(name=="snapshot-roundtrip") snapshotRoundtrip();
    else if(name=="snapshot-failures") snapshotFailures();
    else if(name=="input-roundtrip") inputRoundtrip();
    else if(name=="input-validation") inputValidation();
    else if(name=="input-disconnect") inputDisconnect();
    else if(name=="save-input-hold") saveInputHold();
    else if(name=="poses-roundtrip") posesRoundtrip();
    else if(name=="poses-validation") posesValidation();
    else if(name=="rejoin-queues") rejoinQueues();
    else if(name=="rejoin-stale") rejoinStale();
    else throw std::runtime_error("Unknown connection test");
    std::cout << "PASS: " << name << '\n';
    return 0;
    }
  catch(const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
    }
  }
