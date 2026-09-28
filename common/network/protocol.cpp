#include "protocol.h"

#include <algorithm>
#include <utility>

namespace Coop {
namespace {

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
    case MessageType::Input:    return 8;
    case MessageType::StateAck: return 8;
    case MessageType::Reject:   return 2;
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
  if(auto input = std::get_if<Input>(&message.payload)) {
    if((input->buttons & ~KnownButtons)!=0 ||
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
  if(get(bytes,4,4)!=HeaderBytes+bodySize(type))
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
      break;
    case MessageType::Input:
      message.payload = Input{uint32_t(get(bytes,44,4)),getSigned(bytes,48),getSigned(bytes,50)};
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
  invalid(ProtocolErrorCode::Type,"Missing co-op payload");
  }

Bytes encode(const Message& message) {
  const auto type = messageType(message);
  validateHeader(message,type);
  validatePayload(message);
  Bytes bytes;
  bytes.reserve(HeaderBytes+bodySize(type));
  bytes.insert(bytes.end(),{'O','G','C','P'});
  put(bytes,HeaderBytes+bodySize(type),4);
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
      break;
    case MessageType::Input: {
      const auto& input = std::get<Input>(message.payload);
      put(bytes,input.buttons,4);
      put(bytes,uint16_t(input.lookX),2);
      put(bytes,uint16_t(input.lookY),2);
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
