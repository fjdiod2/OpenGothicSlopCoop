#include "protocol.h"

#include <algorithm>
#include <utility>
#include <bit>
#include <cmath>
#include <limits>

namespace Coop {
namespace {
static_assert(sizeof(float)==4 && std::numeric_limits<float>::is_iec559);

[[noreturn]] void invalid(ProtocolErrorCode code, const char* message) {
  throw ProtocolError(code,message);
  }

void checkLimits(QueueLimits limits) {
  if(limits.bytes<HeaderBytes || limits.bytes>MaxQueueBytes ||
     limits.messages==0 || limits.messages>MaxQueueMessages)
    throw std::invalid_argument("Invalid co-op queue limits");
  }

void put(Bytes& bytes, uint64_t value, size_t width) {
  for(size_t i=width; i>0; --i)
    bytes.push_back(uint8_t(value>>((i-1)*8)));
  }

uint64_t get(std::span<const uint8_t> bytes, size_t offset, size_t width) {
  uint64_t value = 0;
  for(size_t i=0; i<width; ++i)
    value = (value<<8) | bytes[offset+i];
  return value;
  }

int16_t getSigned(std::span<const uint8_t> bytes, size_t offset) {
  const auto value = int32_t(get(bytes,offset,2));
  return int16_t(value<0x8000 ? value : value-0x10000);
  }

size_t bodySize(MessageType type) {
  switch(type) {
    case MessageType::Hello:    return 64;
    case MessageType::Accepted: return 64;
    case MessageType::Welcome:  return 0;
    case MessageType::Input:    return 24;
    case MessageType::StateAck: return 8;
    case MessageType::Reject:   return 2;
    case MessageType::SnapshotBegin: return 40;
    case MessageType::SnapshotChunk: return 8; // Offset prefix; bounded data follows.
    case MessageType::SnapshotEnd:
    case MessageType::SnapshotReady:
    case MessageType::PoseAck: return 0;
    case MessageType::Poses: return 26+2*(49+MaxAnimationLayers*(AnimationNameBytes+21));
    }
  invalid(ProtocolErrorCode::Type,"Unknown co-op message type");
  }

void validateHeader(const Message& message, MessageType type) {
  bool valid = false;
  switch(type) {
    case MessageType::Hello:
    case MessageType::Reject:
      valid = message.generation==0 && message.player==0 && message.sequence==0 && message.revision==0;
      break;
    case MessageType::Welcome:
    case MessageType::StateAck:
    case MessageType::SnapshotBegin:
    case MessageType::SnapshotChunk:
    case MessageType::SnapshotEnd:
    case MessageType::SnapshotReady:
    case MessageType::Poses:
    case MessageType::PoseAck:
      valid = message.generation!=0 && message.player!=0 && message.sequence==0 && message.revision!=0;
      break;
    case MessageType::Input:
      valid = message.generation!=0 && message.player!=0 && message.sequence!=0 && message.revision==0;
      break;
    case MessageType::Accepted:
      valid = message.generation!=0 && message.player==0 && message.sequence==0 && message.revision==0;
      break;
    }
  if(!valid)
    invalid(ProtocolErrorCode::Fields,"Invalid co-op message header fields");
  }

void validatePayload(const Message& message) {
  if(auto poses = std::get_if<Poses>(&message.payload)) {
    if(poses->pickups.size()>MaxPickups)
      invalid(ProtocolErrorCode::Fields,"Too many pickup transfers");
    for(size_t i=0; i<poses->pickups.size(); ++i) {
      const auto& p = poses->pickups[i];
      if(p.player<1 || p.player>2 || p.actor==0 || p.item==0 || p.actor==p.item ||
         p.actor!=poses->actors[p.player-1].object || p.instance>UINT32_MAX ||
         p.amount==0 || p.before>UINT32_MAX || p.amount>UINT32_MAX-p.before || p.after!=p.before+p.amount)
        invalid(ProtocolErrorCode::Fields,"Invalid pickup transfer");
      for(size_t j=0; j<i; ++j)
        if(poses->pickups[j].item==p.item)
          invalid(ProtocolErrorCode::Fields,"Repeated pickup item");
      }
    if(poses->world==0 || poses->actors[0].player!=1 || poses->actors[1].player!=2 ||
       poses->actors[0].object==0 || poses->actors[1].object==0 || poses->actors[0].object==poses->actors[1].object)
      invalid(ProtocolErrorCode::Fields,"Invalid pose identities");
    const auto bounded = [](float value, float max) { return std::isfinite(value) && std::abs(value)<=max; };
    for(auto& actor:poses->actors) {
      for(float value:actor.position)
        if(!bounded(value,1e7f)) invalid(ProtocolErrorCode::Fields,"Invalid pose position");
      if(!bounded(actor.yaw,1e6f) || !bounded(actor.pitch,90.f) || !bounded(actor.roll,360.f) ||
         !bounded(actor.head[0],360.f) || !bounded(actor.head[1],360.f) || actor.layers.size()>MaxAnimationLayers)
        invalid(ProtocolErrorCode::Fields,"Invalid pose orientation or layers");
      for(auto& layer:actor.layers) {
        if(layer.name.empty() || layer.name.size()>AnimationNameBytes || layer.started>poses->tick ||
           layer.body>0x1ffff || layer.combination>9)
          invalid(ProtocolErrorCode::Fields,"Invalid animation layer");
        for(char c:layer.name)
          if(c<32 || c>126) invalid(ProtocolErrorCode::Fields,"Invalid animation name");
        }
      }
    }
  if(auto begin = std::get_if<SnapshotBegin>(&message.payload)) {
    if(begin->bytes==0 || begin->bytes>MaxSnapshotBytes)
      invalid(ProtocolErrorCode::Size,"Invalid snapshot size");
    }
  if(auto chunk = std::get_if<SnapshotChunk>(&message.payload)) {
    if(chunk->data.empty() || chunk->data.size()>SnapshotChunkBytes ||
       chunk->offset>MaxSnapshotBytes || chunk->data.size()>MaxSnapshotBytes-chunk->offset)
      invalid(ProtocolErrorCode::Size,"Invalid snapshot chunk");
    }
  if(auto input = std::get_if<Input>(&message.payload)) {
    if((input->pickupWorld==0)!=(input->pickupItem==0) || (input->buttons & ~KnownButtons)!=0 ||
       input->lookX < -MaxLookDelta || input->lookX > MaxLookDelta ||
       input->lookY < -MaxLookDelta || input->lookY > MaxLookDelta)
      invalid(ProtocolErrorCode::Fields,"Invalid co-op input payload");
    }
  if(auto reject = std::get_if<Reject>(&message.payload)) {
    switch(reject->reason) {
      case RejectReason::Version:
      case RejectReason::Engine:
      case RejectReason::GameData:
      case RejectReason::Full:
      case RejectReason::Session:
        return;
      }
    invalid(ProtocolErrorCode::Fields,"Unknown co-op rejection reason");
    }
  }

size_t readPrefix(std::span<const uint8_t> bytes) {
  if(bytes[0]!='O' || bytes[1]!='G' || bytes[2]!='C' || bytes[3]!='P')
    invalid(ProtocolErrorCode::Magic,"Invalid co-op frame magic");
  const auto length = get(bytes,4,4);
  if(length<HeaderBytes || length>MaxFrameBytes)
    invalid(ProtocolErrorCode::Size,"Co-op frame length outside limits");
  return size_t(length);
  }

Message readHeader(std::span<const uint8_t> bytes) {
  if(get(bytes,8,2)!=ProtocolVersion)
    invalid(ProtocolErrorCode::Version,"Unsupported co-op protocol version");
  const auto type = MessageType(get(bytes,10,2));
  const auto length = get(bytes,4,4);
  const auto fixed = HeaderBytes+bodySize(type);
  const bool validLength = type==MessageType::SnapshotChunk ? (length>fixed && length<=fixed+SnapshotChunkBytes) :
                           type==MessageType::Poses ? (length>=fixed && length<=fixed+MaxPickups*PickupBytes &&
                                                      (length-fixed)%PickupBytes==0) : length==fixed;
  if(!validLength)
    invalid(ProtocolErrorCode::Size,"Co-op payload length does not match message type");
  Message message;
  message.generation = get(bytes,12,8);
  message.player     = get(bytes,20,8);
  message.sequence   = get(bytes,28,8);
  message.revision   = get(bytes,36,8);
  validateHeader(message,type);
  switch(type) {
    case MessageType::Hello:    message.payload = Hello{};    break;
    case MessageType::Welcome:  message.payload = Welcome{};  break;
    case MessageType::Input:    message.payload = Input{};    break;
    case MessageType::StateAck: message.payload = StateAck{}; break;
    case MessageType::Reject:   message.payload = Reject{};   break;
    case MessageType::Accepted: message.payload = Accepted{}; break;
    case MessageType::SnapshotBegin: message.payload = SnapshotBegin{}; break;
    case MessageType::SnapshotChunk: message.payload = SnapshotChunk{}; break;
    case MessageType::SnapshotEnd: message.payload = SnapshotEnd{}; break;
    case MessageType::SnapshotReady: message.payload = SnapshotReady{}; break;
    case MessageType::Poses: message.payload = Poses{}; break;
    case MessageType::PoseAck: message.payload = PoseAck{}; break;
    }
  return message;
  }

Message decode(std::span<const uint8_t> bytes) {
  auto message = readHeader(bytes);
  switch(messageType(message)) {
    case MessageType::Hello:
    case MessageType::Accepted: {
      auto& hello = std::holds_alternative<Hello>(message.payload) ? std::get<Hello>(message.payload) :
                                                                   std::get<Accepted>(message.payload).identity;
      std::copy_n(bytes.begin()+HeaderBytes,32,hello.engine.begin());
      std::copy_n(bytes.begin()+HeaderBytes+32,32,hello.gameData.begin());
      break;
      }
    case MessageType::Welcome:
    case MessageType::SnapshotEnd:
    case MessageType::SnapshotReady:
    case MessageType::PoseAck:
      break;
    case MessageType::Poses: {
      Poses poses;
      size_t offset = HeaderBytes;
      const auto integer = [&](size_t width) { auto v = get(bytes,offset,width); offset += width; return v; };
      const auto number = [&] { return std::bit_cast<float>(uint32_t(integer(4))); };
      poses.world = integer(8); poses.tick = integer(8); poses.appliedInput = integer(8);
      for(auto& actor:poses.actors) {
        actor.player = integer(8); actor.object = integer(8);
        for(auto& value:actor.position) value = number();
        actor.yaw = number(); actor.pitch = number(); actor.roll = number();
        for(auto& value:actor.head) value = number();
        const auto count = integer(1);
        if(count>MaxAnimationLayers) invalid(ProtocolErrorCode::Fields,"Too many pose layers");
        for(size_t i=0; i<MaxAnimationLayers; ++i) {
          AnimationLayer layer;
          bool padding = false;
          for(size_t n=0; n<AnimationNameBytes; ++n) {
            const auto c = integer(1);
            if(c==0) padding = true;
            else if(padding) invalid(ProtocolErrorCode::Fields,"Invalid animation name padding");
            else layer.name.push_back(char(c));
            }
          layer.started = integer(8); layer.blend = integer(8);
          layer.body = uint32_t(integer(4)); layer.combination = uint8_t(integer(1));
          if(i<count) actor.layers.push_back(std::move(layer));
          else if(layer!=AnimationLayer{}) invalid(ProtocolErrorCode::Fields,"Nonzero unused pose layer");
          }
        }
      const auto count = integer(2);
      if(count>MaxPickups || offset+count*PickupBytes!=bytes.size())
        invalid(ProtocolErrorCode::Size,"Invalid pickup transfer count");
      for(size_t i=0; i<count; ++i) {
        Pickup p;
        p.player=integer(8); p.actor=integer(8); p.item=integer(8); p.instance=integer(8);
        p.amount=integer(8); p.before=integer(8); p.after=integer(8);
        poses.pickups.push_back(p);
        }
      message.payload = std::move(poses);
      break;
      }
    case MessageType::SnapshotBegin: {
      SnapshotBegin begin;
      begin.bytes = get(bytes,44,8);
      std::copy_n(bytes.begin()+52,32,begin.digest.begin());
      message.payload = begin;
      break;
      }
    case MessageType::SnapshotChunk:
      message.payload = SnapshotChunk{get(bytes,44,8),Bytes(bytes.begin()+52,bytes.end())};
      break;
    case MessageType::Input:
      message.payload = Input{uint32_t(get(bytes,44,4)),getSigned(bytes,48),getSigned(bytes,50),get(bytes,52,8),get(bytes,60,8)};
      break;
    case MessageType::StateAck:
      message.payload = StateAck{get(bytes,44,8)};
      break;
    case MessageType::Reject:
      message.payload = Reject{RejectReason(get(bytes,44,2))};
      break;
    }
  validatePayload(message);
  return message;
  }

}

ProtocolError::ProtocolError(ProtocolErrorCode code, const char* what)
  :std::runtime_error(what),value(code) {
  }

MessageType messageType(const Message& message) {
  if(std::holds_alternative<Hello>(message.payload))    return MessageType::Hello;
  if(std::holds_alternative<Welcome>(message.payload))  return MessageType::Welcome;
  if(std::holds_alternative<Input>(message.payload))    return MessageType::Input;
  if(std::holds_alternative<StateAck>(message.payload)) return MessageType::StateAck;
  if(std::holds_alternative<Reject>(message.payload))   return MessageType::Reject;
  if(std::holds_alternative<Accepted>(message.payload)) return MessageType::Accepted;
  if(std::holds_alternative<SnapshotBegin>(message.payload)) return MessageType::SnapshotBegin;
  if(std::holds_alternative<SnapshotChunk>(message.payload)) return MessageType::SnapshotChunk;
  if(std::holds_alternative<SnapshotEnd>(message.payload)) return MessageType::SnapshotEnd;
  if(std::holds_alternative<SnapshotReady>(message.payload)) return MessageType::SnapshotReady;
  if(std::holds_alternative<Poses>(message.payload)) return MessageType::Poses;
  if(std::holds_alternative<PoseAck>(message.payload)) return MessageType::PoseAck;
  invalid(ProtocolErrorCode::Type,"Missing co-op payload");
  }

Bytes encode(const Message& message) {
  const auto type = messageType(message);
  validateHeader(message,type);
  validatePayload(message);
  Bytes bytes;
  const auto chunk = std::get_if<SnapshotChunk>(&message.payload);
  const auto poses = std::get_if<Poses>(&message.payload);
  const auto length = HeaderBytes+bodySize(type)+(chunk ? chunk->data.size() : 0)+(poses ? poses->pickups.size()*PickupBytes : 0);
  bytes.reserve(length);
  bytes.insert(bytes.end(),{'O','G','C','P'});
  put(bytes,length,4);
  put(bytes,ProtocolVersion,2);
  put(bytes,uint16_t(type),2);
  put(bytes,message.generation,8);
  put(bytes,message.player,8);
  put(bytes,message.sequence,8);
  put(bytes,message.revision,8);
  switch(type) {
    case MessageType::Hello:
    case MessageType::Accepted: {
      const auto& hello = std::holds_alternative<Hello>(message.payload) ? std::get<Hello>(message.payload) :
                                                                         std::get<Accepted>(message.payload).identity;
      bytes.insert(bytes.end(),hello.engine.begin(),hello.engine.end());
      bytes.insert(bytes.end(),hello.gameData.begin(),hello.gameData.end());
      break;
      }
    case MessageType::Welcome:
    case MessageType::SnapshotEnd:
    case MessageType::SnapshotReady:
    case MessageType::PoseAck:
      break;
    case MessageType::Poses: {
      const auto& poses = std::get<Poses>(message.payload);
      const auto number = [&](float v) { put(bytes,std::bit_cast<uint32_t>(v),4); };
      put(bytes,poses.world,8); put(bytes,poses.tick,8); put(bytes,poses.appliedInput,8);
      for(auto& actor:poses.actors) {
        put(bytes,actor.player,8); put(bytes,actor.object,8);
        for(float value:actor.position) number(value);
        number(actor.yaw); number(actor.pitch); number(actor.roll);
        for(float value:actor.head) number(value);
        put(bytes,actor.layers.size(),1);
        for(size_t i=0; i<MaxAnimationLayers; ++i) {
          if(i>=actor.layers.size()) { bytes.resize(bytes.size()+AnimationNameBytes+21,0); continue; }
          auto& layer = actor.layers[i];
          bytes.insert(bytes.end(),layer.name.begin(),layer.name.end());
          bytes.resize(bytes.size()+AnimationNameBytes-layer.name.size(),0);
          put(bytes,layer.started,8); put(bytes,layer.blend,8); put(bytes,layer.body,4); put(bytes,layer.combination,1);
          }
        }
      put(bytes,poses.pickups.size(),2);
      for(const auto& p:poses.pickups) {
        put(bytes,p.player,8); put(bytes,p.actor,8); put(bytes,p.item,8); put(bytes,p.instance,8);
        put(bytes,p.amount,8); put(bytes,p.before,8); put(bytes,p.after,8);
        }
      break;
      }
    case MessageType::SnapshotBegin: {
      const auto& begin = std::get<SnapshotBegin>(message.payload);
      put(bytes,begin.bytes,8);
      bytes.insert(bytes.end(),begin.digest.begin(),begin.digest.end());
      break;
      }
    case MessageType::SnapshotChunk:
      put(bytes,chunk->offset,8);
      bytes.insert(bytes.end(),chunk->data.begin(),chunk->data.end());
      break;
    case MessageType::Input: {
      const auto& input = std::get<Input>(message.payload);
      put(bytes,input.buttons,4);
      put(bytes,uint16_t(input.lookX),2);
      put(bytes,uint16_t(input.lookY),2);
      put(bytes,input.pickupWorld,8);
      put(bytes,input.pickupItem,8);
      break;
      }
    case MessageType::StateAck:
      put(bytes,std::get<StateAck>(message.payload).appliedInput,8);
      break;
    case MessageType::Reject:
      put(bytes,uint16_t(std::get<Reject>(message.payload).reason),2);
      break;
    }
  return bytes;
  }

Decoder::Decoder(QueueLimits limits):limits(limits) {
  checkLimits(limits);
  partial.reserve(HeaderBytes);
  }

void Decoder::checkFailure() const {
  if(failure)
    invalid(*failure,"Co-op decoder is unusable after an earlier failure");
  }

void Decoder::feed(std::span<const uint8_t> bytes) {
  checkFailure();
  if(ended)
    invalid(ProtocolErrorCode::Closed,"Co-op bytes received after EOF");
  try {
    while(!bytes.empty()) {
      const size_t count = std::min(target-partial.size(),bytes.size());
      if(count>limits.bytes-bufferedBytes())
        invalid(ProtocolErrorCode::QueueLimit,"Co-op receive byte limit reached");
      const auto chunk = bytes.first(count);
      partial.insert(partial.end(),chunk.begin(),chunk.end());
      bytes = bytes.subspan(count);
      if(partial.size()!=target)
        continue;
      if(target==8) {
        readPrefix(partial); // Reject oversized lengths before buffering a body.
        target = HeaderBytes;
        continue;
        }
      if(target==HeaderBytes) {
        readHeader(partial); // Validate type, fields and exact length immediately.
        target = readPrefix(partial);
        partial.reserve(target);
        if(partial.size()!=target)
          continue;
        }
      if(ready.size()>=limits.messages)
        invalid(ProtocolErrorCode::QueueLimit,"Co-op receive message limit reached");
      ready.push_back(decode(partial));
      queuedBytes += partial.size();
      partial.clear();
      target = 8;
      }
    }
  catch(const ProtocolError& error) {
    failure = error.code();
    ready.clear();
    partial.clear();
    queuedBytes = 0;
    throw;
    }
  }

void Decoder::finish() {
  checkFailure();
  ended = true;
  if(!partial.empty()) {
    failure = ProtocolErrorCode::Truncated;
    ready.clear();
    partial.clear();
    queuedBytes = 0;
    invalid(*failure,"Co-op stream ended inside a frame");
    }
  }

std::optional<Message> Decoder::pop() {
  checkFailure();
  if(ready.empty())
    return std::nullopt;
  auto message = std::move(ready.front());
  ready.pop_front();
  queuedBytes -= HeaderBytes+bodySize(messageType(message));
  if(auto chunk = std::get_if<SnapshotChunk>(&message.payload))
    queuedBytes -= chunk->data.size();
  return message;
  }

WriteQueue::WriteQueue(QueueLimits limits):limits(limits) {
  checkLimits(limits);
  }

bool WriteQueue::enqueue(const Message& message) {
  auto frame = encode(message);
  if(frames.size()>=limits.messages || frame.size()>limits.bytes-storedBytes)
    return false;
  const size_t size = frame.size();
  frames.push_back(std::move(frame));
  storedBytes += size;
  remainingBytes += size;
  return true;
  }

std::span<const uint8_t> WriteQueue::front() const {
  if(frames.empty())
    return {};
  return std::span<const uint8_t>(frames.front()).subspan(offset);
  }

void WriteQueue::consume(size_t bytes) {
  if(bytes>front().size())
    invalid(ProtocolErrorCode::WriteCount,"Co-op write acknowledgement exceeds offered bytes");
  if(bytes==0)
    return;
  offset += bytes;
  remainingBytes -= bytes;
  if(offset==frames.front().size()) {
    storedBytes -= frames.front().size();
    frames.pop_front();
    offset = 0;
    }
  }

}
