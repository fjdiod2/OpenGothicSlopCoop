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

void timeout() {
  Session host, guest;
  host.start({Role::Host,0},identity(),0);
  REQUIRE(host.status().state=="listening");
  Socket silent;
  silent.connect(host.status().port);
  host.tick(1);
  REQUIRE(host.status().peers==1);
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
  REQUIRE(bounded.status().peers==Session::MaxPeers);
  REQUIRE(bounded.status().reason=="pending_limit");
  bounded.tick(1+Session::HandshakeTimeoutMs);
  REQUIRE(bounded.status().peers==0);
  REQUIRE(bounded.status().timeouts==Session::MaxPeers);
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
    else throw std::runtime_error("Unknown connection test");
    std::cout << "PASS: " << name << '\n';
    return 0;
    }
  catch(const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
    }
  }
