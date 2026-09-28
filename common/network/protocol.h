#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <span>
#include <stdexcept>
#include <variant>
#include <vector>

namespace Coop {

using Bytes = std::vector<uint8_t>;
using Fingerprint = std::array<uint8_t,32>;

inline constexpr uint16_t ProtocolVersion = 1;
inline constexpr size_t HeaderBytes        = 44;
inline constexpr size_t MaxFrameBytes      = 64*1024;
inline constexpr size_t MaxQueueBytes      = 256*1024;
inline constexpr size_t MaxQueueMessages   = 256;

enum class MessageType : uint16_t {
  Hello = 1, Welcome = 2, Input = 3, StateAck = 4, Reject = 5
  };

enum class RejectReason : uint16_t {
  Version = 1, Engine = 2, GameData = 3, Full = 4, Session = 5
  };

enum Button : uint32_t {
  Forward = 1u<<0, Backward = 1u<<1, TurnLeft = 1u<<2, TurnRight = 1u<<3,
  StrafeLeft = 1u<<4, StrafeRight = 1u<<5, Jump = 1u<<6, Walk = 1u<<7
  };
inline constexpr uint32_t KnownButtons = 0xff;
inline constexpr int16_t MaxLookDelta = 1000;

struct Hello {
  Fingerprint engine = {};
  Fingerprint gameData = {};
  bool operator==(const Hello&) const = default;
  };
struct Welcome {
  bool operator==(const Welcome&) const = default;
  };
struct Input {
  uint32_t buttons = 0;
  int16_t  lookX = 0, lookY = 0;
  bool operator==(const Input&) const = default;
  };
struct StateAck {
  uint64_t appliedInput = 0;
  bool operator==(const StateAck&) const = default;
  };
struct Reject {
  RejectReason reason = RejectReason::Session;
  bool operator==(const Reject&) const = default;
  };

using Payload = std::variant<Hello,Welcome,Input,StateAck,Reject>;

struct Message {
  uint64_t generation = 0;
  uint64_t player     = 0;
  uint64_t sequence   = 0;
  uint64_t revision   = 0;
  Payload  payload   = Hello{};
  bool operator==(const Message&) const = default;
  };

enum class ProtocolErrorCode {
  Magic, Size, Version, Type, Fields, QueueLimit, Truncated, Closed, WriteCount
  };

class ProtocolError final : public std::runtime_error {
  public:
    ProtocolError(ProtocolErrorCode code, const char* what);
    ProtocolErrorCode code() const { return value; }

  private:
    ProtocolErrorCode value;
  };

struct QueueLimits {
  size_t bytes    = MaxQueueBytes;
  size_t messages = MaxQueueMessages;
  };

// See docs/coop-protocol.md for byte layout, directions and session semantics.
MessageType messageType(const Message& message);
Bytes       encode(const Message& message);

class Decoder final {
  public:
    explicit Decoder(QueueLimits limits = {});
    // Consumes arbitrary TCP read boundaries. Any protocol/limit error poisons
    // the decoder and discards pending messages; create a new one per connection.
    void                   feed(std::span<const uint8_t> bytes);
    void                   finish(); // EOF: rejects an incomplete frame.
    std::optional<Message> pop();
    size_t                 queuedMessages() const { return ready.size(); }
    size_t                 bufferedBytes() const { return queuedBytes+partial.size(); }
    bool                   failed() const { return failure.has_value(); }

  private:
    void checkFailure() const;

    QueueLimits                      limits;
    Bytes                            partial;
    size_t                           target = 8;
    size_t                           queuedBytes = 0;
    std::deque<Message>              ready;
    std::optional<ProtocolErrorCode>  failure;
    bool                             ended = false;
  };

class WriteQueue final {
  public:
    explicit WriteQueue(QueueLimits limits = {});
    // False means backpressure: nothing was queued. Caller must retry or close,
    // never silently drop an input/state message. Invalid messages throw.
    bool                     enqueue(const Message& message);
    std::span<const uint8_t> front() const;
    // Acknowledge exactly the bytes accepted by send(). Zero leaves it intact
    // (would-block); socket EOF/errors must be handled by the connection owner.
    void                     consume(size_t bytes);
    size_t                   pendingBytes() const { return remainingBytes; }
    size_t                   retainedBytes() const { return storedBytes; }
    size_t                   queuedMessages() const { return frames.size(); }

  private:
    QueueLimits        limits;
    std::deque<Bytes>  frames;
    size_t             offset = 0;
    size_t             storedBytes = 0;
    size_t             remainingBytes = 0;
  };

}
