#include "network/protocol.h"

#include <algorithm>
#include <functional>
#include <iostream>
#include <limits>
#include <random>
#include <string>
#include <utility>

using namespace Coop;

namespace {

#define REQUIRE(expression) do { if(!(expression)) throw std::runtime_error( \
    std::string(__FILE__)+":"+std::to_string(__LINE__)+": " #expression); } while(false)

template<class F>
void error(ProtocolErrorCode code, F action) {
  try {
    action();
    }
  catch(const ProtocolError& e) {
    REQUIRE(e.code()==code);
    return;
    }
  throw std::runtime_error("Expected protocol error was not raised");
  }

Bytes hex(const std::string& text) {
  Bytes bytes;
  REQUIRE(text.size()%2==0);
  for(size_t i=0; i<text.size(); i+=2)
    bytes.push_back(uint8_t(std::stoul(text.substr(i,2),nullptr,16)));
  return bytes;
  }

Message input(uint64_t sequence=9) {
  return {0x0102030405060708,2,sequence,0,Input{Forward|Walk,-5,100}};
  }

// Independent literal from docs/coop-protocol.md, not constructed by the codec.
Bytes goldenInput() {
  return hex("4f4743500000003400030003"
             "0102030405060708" "0000000000000002"
             "0000000000000009" "0000000000000000"
             "00000081fffb0064");
  }

std::vector<Message> examples() {
  Hello hello;
  for(size_t i=0; i<hello.engine.size(); ++i) {
    hello.engine[i] = uint8_t(i);
    hello.gameData[i] = uint8_t(255-i);
    }
  constexpr auto maximum = std::numeric_limits<uint64_t>::max();
  return {
    {0,0,0,0,hello},
    {maximum,0,0,0,Accepted{hello}},
    {maximum,2,0,maximum,Welcome{}},
    input(),
    {maximum,maximum,maximum,0,Input{KnownButtons,-1000,1000}},
    {1,2,0,2,StateAck{0}},
    {maximum,2,0,maximum,StateAck{maximum}},
    {0,0,0,0,Reject{RejectReason::Version}},
    {0,0,0,0,Reject{RejectReason::Engine}},
    {0,0,0,0,Reject{RejectReason::GameData}},
    {0,0,0,0,Reject{RejectReason::Full}},
    {0,0,0,0,Reject{RejectReason::Session}},
    {4,2,0,1,SnapshotBegin{100,{}}},
    {4,2,0,1,SnapshotChunk{0,{1,2,3}}},
    {4,2,0,1,SnapshotEnd{}},
    {4,2,0,1,SnapshotReady{}}
    };
  }

Bytes join(const std::vector<Message>& messages) {
  Bytes bytes;
  for(auto& message:messages) {
    const auto frame = encode(message);
    bytes.insert(bytes.end(),frame.begin(),frame.end());
    }
  return bytes;
  }

void drain(Decoder& decoder, const std::vector<Message>& expected) {
  for(auto& message:expected) {
    const auto next = decoder.pop();
    REQUIRE(next && *next==message);
    }
  REQUIRE(!decoder.pop());
  REQUIRE(decoder.queuedMessages()==0);
  }

void golden() {
  const auto bytes = goldenInput();
  REQUIRE(bytes.size()==52);
  REQUIRE(encode(input())==bytes);
  Decoder decoder;
  decoder.feed(bytes);
  decoder.finish();
  drain(decoder,{input()});
  }

void messageTypes() {
  for(auto& message:examples()) {
    Decoder decoder;
    const auto bytes = encode(message);
    decoder.feed(bytes);
    REQUIRE(decoder.bufferedBytes()==bytes.size());
    decoder.finish();
    drain(decoder,{message});
    REQUIRE(decoder.bufferedBytes()==0);
    }
  // A second literal covers the variable fingerprints and byte order of a
  // different message type, so matching encode/decode bugs cannot hide it.
  const auto hello = hex("4f4743500000006c00030001"
                         "00000000000000000000000000000000"
                         "00000000000000000000000000000000"
                         "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f"
                         "fffefdfcfbfaf9f8f7f6f5f4f3f2f1f0efeeedecebeae9e8e7e6e5e4e3e2e1e0");
  REQUIRE(encode(examples().front())==hello);
  }

void fragments() {
  for(auto& message:examples()) {
    const auto bytes = encode(message);
    for(size_t split=0; split<=bytes.size(); ++split) {
      Decoder decoder;
      decoder.feed(std::span(bytes).first(split));
      REQUIRE(decoder.queuedMessages()==(split==bytes.size() ? 1u : 0u));
      decoder.feed({});
      decoder.feed(std::span(bytes).subspan(split));
      decoder.finish();
      drain(decoder,{message});
      }
    Decoder decoder;
    for(auto byte:bytes)
      decoder.feed(std::span(&byte,1));
    drain(decoder,{message});
    }
  }

void combined() {
  const auto messages = examples();
  const auto bytes = join(messages);
  Decoder decoder;
  decoder.feed(bytes);
  REQUIRE(decoder.queuedMessages()==messages.size());
  REQUIRE(decoder.bufferedBytes()==bytes.size());
  decoder.finish();
  drain(decoder,messages);
  for(size_t split=0; split<=bytes.size(); ++split) {
    Decoder fragmented;
    fragmented.feed(std::span(bytes).first(split));
    fragmented.feed(std::span(bytes).subspan(split));
    fragmented.finish();
    drain(fragmented,messages);
    }
  }

void malformedHeader() {
  const auto original = goldenInput();
  auto reject = [](const Bytes& bytes, ProtocolErrorCode code) {
    Decoder decoder;
    error(code,[&] { decoder.feed(bytes); });
    REQUIRE(decoder.failed());
    REQUIRE(decoder.bufferedBytes()==0);
    REQUIRE(decoder.queuedMessages()==0);
    error(code,[&] { decoder.pop(); });
    error(code,[&] { decoder.feed(goldenInput()); });
    error(code,[&] { decoder.finish(); });
    };
  for(size_t offset=0; offset<4; ++offset) {
    auto bytes = original;
    bytes[offset] ^= 0xff;
    bytes.resize(8);
    reject(bytes,ProtocolErrorCode::Magic);
    }
  for(const auto& length: {"00000000","00000007","0000002b","00010001","ffffffff"}) {
    auto bytes = original;
    const auto field = hex(length);
    std::copy(field.begin(),field.end(),bytes.begin()+4);
    bytes.resize(8); // Rejected from the prefix, without receiving a payload.
    reject(bytes,ProtocolErrorCode::Size);
    }
  for(const auto& length: {"0000002c","00010000"}) {
    auto bytes = original;
    const auto field = hex(length);
    std::copy(field.begin(),field.end(),bytes.begin()+4);
    bytes.resize(HeaderBytes); // Globally allowed size, but not for an Input.
    reject(bytes,ProtocolErrorCode::Size);
    }
  for(const auto& [offset,code]:std::vector<std::pair<size_t,ProtocolErrorCode>> {
        {8,ProtocolErrorCode::Version},{9,ProtocolErrorCode::Version},
        {10,ProtocolErrorCode::Type},{11,ProtocolErrorCode::Type}}) {
    auto bytes = original;
    bytes[offset] ^= 0x40;
    bytes.resize(HeaderBytes);
    reject(bytes,code);
    }
  for(auto& message:examples()) {
    auto bytes = encode(message);
    if(std::holds_alternative<SnapshotChunk>(message.payload)) bytes[7] = 52; else
    ++bytes[7]; // Within the global cap, but wrong for this message schema.
    bytes.resize(HeaderBytes);
    reject(bytes,ProtocolErrorCode::Size);
    const bool unbound = std::holds_alternative<Hello>(message.payload) ||
                         std::holds_alternative<Reject>(message.payload);
    const bool isInput = std::holds_alternative<Input>(message.payload);
    for(size_t field=0; field<4; ++field) {
      const size_t offset = 12+field*8;
      bytes = encode(message);
      std::fill_n(bytes.begin()+std::ptrdiff_t(offset),8,uint8_t(0));
      const bool needsZero = unbound || (std::holds_alternative<Accepted>(message.payload) ? field!=0 :
                                        isInput ? field==3 : field==2);
      if(needsZero)
        bytes[offset+7] = 1;
      bytes.resize(HeaderBytes);
      reject(bytes,ProtocolErrorCode::Fields);
      }
    }
  // A valid frame before a bad one must not remain available after failure.
  auto bytes = original;
  auto invalid = original;
  invalid[0] = 0;
  bytes.insert(bytes.end(),invalid.begin(),invalid.end());
  reject(bytes,ProtocolErrorCode::Magic);
  }

void malformedPayload() {
  for(auto [offset,value]: std::vector<std::pair<size_t,uint8_t>>{
        {44,1},{46,1},{48,0x04},{48,0x80},{50,0x04},{50,0x80}}) {
    auto bytes = goldenInput();
    bytes[offset] = value;
    Decoder decoder;
    error(ProtocolErrorCode::Fields,[&] { decoder.feed(bytes); });
    }
  for(const auto& value: {"0000","0006","ffff"}) {
    auto bytes = encode({0,0,0,0,Reject{RejectReason::Full}});
    const auto field = hex(value);
    std::copy(field.begin(),field.end(),bytes.begin()+44);
    Decoder decoder;
    error(ProtocolErrorCode::Fields,[&] { decoder.feed(bytes); });
    }
  // Exactly one unit beyond either signed look limit must be rejected.
  for(auto offset: {48,50}) {
    for(const auto& value: {"03e9","fc17"}) {
      auto bytes = goldenInput();
      const auto field = hex(value);
      std::copy(field.begin(),field.end(),bytes.begin()+offset);
      Decoder decoder;
      error(ProtocolErrorCode::Fields,[&] { decoder.feed(bytes); });
      }
    }
  }

void encodeValidation() {
  for(auto message:examples()) {
    if(std::holds_alternative<Hello>(message.payload) || std::holds_alternative<Reject>(message.payload))
      message.generation = 1;
    else
      message.generation = 0;
    error(ProtocolErrorCode::Fields,[&] { encode(message); });
    }
  for(auto payload: {Input{KnownButtons+1,0,0},Input{0,1001,0},Input{0,0,-1001}}) {
    auto message = input();
    message.payload = payload;
    error(ProtocolErrorCode::Fields,[&] { encode(message); });
    }
  error(ProtocolErrorCode::Fields,[] { encode({0,0,0,0,Reject{RejectReason(0)}}); });
  for(auto size:{uint64_t(0),uint64_t(MaxSnapshotBytes)+1})
    error(ProtocolErrorCode::Size,[&] { encode({1,2,0,1,SnapshotBegin{size,{}}}); });
  for(auto chunk:{SnapshotChunk{0,{}},SnapshotChunk{0,Bytes(SnapshotChunkBytes+1)},
                  SnapshotChunk{MaxSnapshotBytes,Bytes{1}}})
    error(ProtocolErrorCode::Size,[&] { encode({1,2,0,1,chunk}); });
  }

void eof() {
  for(auto& message:examples()) {
    const auto bytes = encode(message);
    for(size_t length=1; length<bytes.size(); ++length) {
      Decoder decoder;
      decoder.feed(std::span(bytes).first(length));
      error(ProtocolErrorCode::Truncated,[&] { decoder.finish(); });
      REQUIRE(decoder.failed());
      REQUIRE(decoder.bufferedBytes()==0);
      error(ProtocolErrorCode::Truncated,[&] { decoder.pop(); });
      }
    }
  Decoder empty;
  empty.finish();
  empty.finish();
  REQUIRE(!empty.pop());
  error(ProtocolErrorCode::Closed,[&] { empty.feed(goldenInput()); });
  Decoder trailing;
  trailing.feed(goldenInput());
  trailing.feed(hex("4f"));
  error(ProtocolErrorCode::Truncated,[&] { trailing.finish(); });
  REQUIRE(trailing.queuedMessages()==0);
  }

void invalidLimits() {
  for(auto limits: {QueueLimits{43,1},QueueLimits{MaxQueueBytes+1,1},
                    QueueLimits{100,0},QueueLimits{100,MaxQueueMessages+1}}) {
    bool decoderRejected = false, writerRejected = false;
    try { Decoder decoder(limits); } catch(const std::invalid_argument&) { decoderRejected = true; }
    try { WriteQueue queue(limits); } catch(const std::invalid_argument&) { writerRejected = true; }
    REQUIRE(decoderRejected && writerRejected);
    }
  }

void receiveLimits() {
  invalidLimits();
  const auto bytes = goldenInput();
  Decoder byCount({MaxQueueBytes,2});
  byCount.feed(bytes);
  byCount.feed(bytes);
  REQUIRE(byCount.queuedMessages()==2);
  error(ProtocolErrorCode::QueueLimit,[&] { byCount.feed(bytes); });
  REQUIRE(byCount.bufferedBytes()==0);
  REQUIRE(byCount.queuedMessages()==0);
  Decoder byBytes({bytes.size()*2,MaxQueueMessages});
  byBytes.feed(bytes);
  byBytes.feed(bytes);
  error(ProtocolErrorCode::QueueLimit,[&] { byBytes.feed(hex("4f")); });
  Decoder partial({bytes.size()+8,MaxQueueMessages});
  partial.feed(bytes);
  partial.feed(std::span(bytes).first(8));
  REQUIRE(partial.bufferedBytes()==bytes.size()+8);
  error(ProtocolErrorCode::QueueLimit,[&] { partial.feed(std::span(bytes).subspan(8)); });
  Decoder reuse({bytes.size(),1});
  for(int i=0; i<1000; ++i) {
    reuse.feed(bytes);
    drain(reuse,{input()});
    REQUIRE(reuse.bufferedBytes()==0);
    }
  Decoder flood;
  Bytes many;
  for(size_t i=0; i<MaxQueueMessages+1; ++i)
    many.insert(many.end(),bytes.begin(),bytes.end());
  error(ProtocolErrorCode::QueueLimit,[&] { flood.feed(many); });
  }

void writeLimits() {
  const auto bytes = goldenInput();
  WriteQueue queue({bytes.size()*2,3});
  REQUIRE(queue.enqueue(input()));
  REQUIRE(queue.enqueue(input(10)));
  REQUIRE(!queue.enqueue(input(11)));
  REQUIRE(queue.pendingBytes()==104 && queue.retainedBytes()==104);
  queue.consume(51);
  REQUIRE(queue.pendingBytes()==53 && queue.retainedBytes()==104);
  REQUIRE(!queue.enqueue(input(11))); // A partial frame still occupies storage.
  queue.consume(1);
  REQUIRE(queue.pendingBytes()==52 && queue.retainedBytes()==52);
  REQUIRE(queue.enqueue(input(11)));
  error(ProtocolErrorCode::WriteCount,[&] { queue.consume(53); });
  REQUIRE(queue.pendingBytes()==104 && queue.queuedMessages()==2);
  auto invalid = input();
  invalid.sequence = 0;
  error(ProtocolErrorCode::Fields,[&] { queue.enqueue(invalid); });
  REQUIRE(queue.pendingBytes()==104 && queue.queuedMessages()==2);
  WriteQueue count({MaxQueueBytes,1});
  REQUIRE(count.enqueue(input()));
  REQUIRE(!count.enqueue(input(10)));
  count.consume(bytes.size());
  REQUIRE(count.front().empty());
  REQUIRE(count.retainedBytes()==0 && count.pendingBytes()==0);
  count.consume(0);
  error(ProtocolErrorCode::WriteCount,[&] { count.consume(1); });
  REQUIRE(count.enqueue(input(10)));
  WriteQueue tooSmall({HeaderBytes,1});
  REQUIRE(!tooSmall.enqueue(input()));
  REQUIRE(tooSmall.front().empty());
  }

void partialWrites() {
  auto messages = examples();
  messages.insert(messages.begin(),input());
  WriteQueue queue;
  REQUIRE(queue.enqueue(messages.front()));
  Bytes received;
  // Simulate an initial short send, then enqueue more while the front is partial.
  received.insert(received.end(),queue.front().begin(),queue.front().begin()+3);
  queue.consume(3);
  for(size_t i=1; i<messages.size(); ++i)
    REQUIRE(queue.enqueue(messages[i]));
  const std::array<size_t,8> accepted = {0,1,2,0,7,31,0,4096};
  size_t iteration = 0;
  while(!queue.front().empty()) {
    const auto offered = queue.front();
    const size_t count = std::min(offered.size(),accepted[iteration++%accepted.size()]);
    const auto pending = queue.pendingBytes();
    received.insert(received.end(),offered.begin(),offered.begin()+std::ptrdiff_t(count));
    queue.consume(count);
    REQUIRE(queue.pendingBytes()==pending-count);
    }
  REQUIRE(queue.retainedBytes()==0 && queue.queuedMessages()==0);
  REQUIRE(received==join(messages));
  const auto golden = goldenInput();
  REQUIRE(std::equal(golden.begin(),golden.end(),received.begin()));
  Decoder decoder;
  decoder.feed(received);
  decoder.finish();
  drain(decoder,messages);
  }

void streamStress() {
  std::mt19937 random(0x4f474350);
  WriteQueue sender({1040,20});
  Decoder receiver({1040,20});
  Bytes stream;
  size_t head = 0;
  uint64_t nextSend = 1, nextReceive = 1;
  constexpr uint64_t count = 10000;
  // Independent bounded write/read schedules exercise backpressure, partial
  // sends, coalesced reads and continual reuse without a real socket or assets.
  for(size_t tick=0; nextReceive<=count; ++tick) {
    REQUIRE(tick<1000000);
    if(nextSend<=count && sender.enqueue(input(nextSend)))
      ++nextSend;
    const auto offered = sender.front();
    const size_t written = std::min(offered.size(),size_t(random()%81));
    stream.insert(stream.end(),offered.begin(),offered.begin()+std::ptrdiff_t(written));
    sender.consume(written);
    const size_t read = std::min(stream.size()-head,size_t(random()%121));
    receiver.feed(std::span(stream).subspan(head,read));
    head += read;
    while(auto message = receiver.pop()) {
      REQUIRE(*message==input(nextReceive));
      ++nextReceive;
      }
    REQUIRE(sender.retainedBytes()<=1040);
    REQUIRE(receiver.bufferedBytes()<=1040);
    if(head==stream.size()) {
      stream.clear();
      head = 0;
      }
    }
  receiver.finish();
  REQUIRE(sender.pendingBytes()==0);
  REQUIRE(head==stream.size());
  }

void malformedStress() {
  std::mt19937 random(0x4e3033);
  const auto seeds = examples();
  for(size_t attempt=0; attempt<10000; ++attempt) {
    auto bytes = encode(seeds[attempt%seeds.size()]);
    for(size_t mutation=0; mutation<1+attempt%5; ++mutation)
      bytes[random()%bytes.size()] ^= uint8_t(1u<<(random()%8));
    if(attempt%3==0)
      bytes.resize(random()%(bytes.size()+1));
    Decoder decoder;
    try {
      size_t offset = 0;
      while(offset<bytes.size()) {
        const size_t count = std::min(bytes.size()-offset,size_t(1+random()%17));
        decoder.feed(std::span(bytes).subspan(offset,count));
        offset += count;
        }
      decoder.finish();
      // Some mutations change legal values. Anything accepted must produce a
      // canonical, bounded frame that is independently safe to decode again.
      while(auto message = decoder.pop()) {
        const auto canonical = encode(*message);
        REQUIRE(canonical.size()<=MaxFrameBytes);
        Decoder check;
        check.feed(canonical);
        check.finish();
        drain(check,{*message});
        }
      }
    catch(const ProtocolError&) {
      REQUIRE(decoder.failed());
      REQUIRE(decoder.bufferedBytes()==0);
      }
    }
  }

}

int main(int argc, char** argv) {
  const std::vector<std::pair<std::string,std::function<void()>>> cases = {
    {"golden",golden},{"message-types",messageTypes},{"fragments",fragments},
    {"combined",combined},{"malformed-header",malformedHeader},{"malformed-payload",malformedPayload},
    {"encode-validation",encodeValidation},{"eof",eof},{"receive-limits",receiveLimits},
    {"write-limits",writeLimits},{"partial-writes",partialWrites},
    {"stream-stress",streamStress},{"malformed-stress",malformedStress}
    };
  if(argc!=2) {
    std::cerr << "Usage: coop-protocol-tests CASE\n";
    return 2;
    }
  for(auto& [name,test]:cases) {
    if(name!=argv[1])
      continue;
    try {
      test();
      std::cout << "PASS: " << name << '\n';
      return 0;
      }
    catch(const std::exception& error) {
      std::cerr << "FAIL: " << name << ": " << error.what() << '\n';
      return 1;
      }
    }
  std::cerr << "Unknown test case: " << argv[1] << '\n';
  return 2;
  }
