#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <span>
#include <stdexcept>
#include <variant>
#include <string>
#include <vector>

namespace Coop {

using Bytes = std::vector<uint8_t>;
using Fingerprint = std::array<uint8_t,32>;

inline constexpr uint16_t ProtocolVersion = 9;
inline constexpr size_t HeaderBytes        = 44;
inline constexpr size_t MaxFrameBytes      = 64*1024;
inline constexpr size_t MaxQueueBytes      = 256*1024;
inline constexpr size_t MaxQueueMessages   = 256;
inline constexpr size_t MaxSnapshotBytes   = 64*1024*1024;
inline constexpr size_t SnapshotChunkBytes = 32*1024;
inline constexpr size_t MaxAnimationLayers = 8;
inline constexpr size_t AnimationNameBytes = 64;
inline constexpr size_t MaxPickups = 16;
inline constexpr size_t MaxStoryBytes = 32*1024;
inline constexpr size_t PickupBytes = 7*8;
inline constexpr size_t ActorPoseBytes = 49+MaxAnimationLayers*(AnimationNameBytes+21);
inline constexpr size_t CombatStateBytes = 66;
inline constexpr size_t MaxCombatNpcs = 32;
inline constexpr size_t TalentCount = 22;
inline constexpr size_t HeldSlotBytes = 32;
inline constexpr size_t PlayerStateBytes = 2*TalentCount*4+3*4+8+HeldSlotBytes;

enum class MessageType : uint16_t {
  Hello = 1, Welcome = 2, Input = 3, StateAck = 4, Reject = 5, Accepted = 6,
  SnapshotBegin = 7, SnapshotChunk = 8, SnapshotEnd = 9, SnapshotReady = 10,
  Poses = 11, PoseAck = 12
  };

enum class RejectReason : uint16_t {
  Version = 1, Engine = 2, GameData = 3, Full = 4, Session = 5
  };

enum Button : uint32_t {
  Forward = 1u<<0, Backward = 1u<<1, TurnLeft = 1u<<2, TurnRight = 1u<<3,
  StrafeLeft = 1u<<4, StrafeRight = 1u<<5, Jump = 1u<<6, Walk = 1u<<7,
  Attack = 1u<<8, Melee = 1u<<9
  };
inline constexpr uint32_t KnownButtons = 0x3ff;
inline constexpr int16_t MaxLookDelta = 1000;

struct Hello {
  Fingerprint engine = {};
  Fingerprint gameData = {};
  bool operator==(const Hello&) const = default;
  };
struct Welcome {
  bool operator==(const Welcome&) const = default;
  };
struct Accepted {
  Hello identity;
  bool operator==(const Accepted&) const = default;
  };
struct Input {
  uint32_t buttons = 0;
  int16_t  lookX = 0, lookY = 0;
  uint64_t pickupWorld = 0, pickupItem = 0; // Both zero, or one explicit item request.
  uint64_t equipmentItem = 0; // Inventory instance + 1; zero means no inventory intent.
  uint8_t  equipmentAction = 0; // 0 none, 1 equip, 2 unequip, 3 consume once.
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

struct SnapshotBegin {
  uint64_t bytes = 0;
  Fingerprint digest = {};
  bool operator==(const SnapshotBegin&) const = default;
  };
struct SnapshotChunk {
  uint64_t offset = 0;
  Bytes data;
  bool operator==(const SnapshotChunk&) const = default;
  };
struct SnapshotEnd { bool operator==(const SnapshotEnd&) const = default; };
struct SnapshotReady { bool operator==(const SnapshotReady&) const = default; };

struct AnimationLayer {
  std::string name;
  uint64_t started = 0, blend = 0;
  uint32_t body = 0;
  uint8_t combination = 0;
  bool operator==(const AnimationLayer&) const = default;
  };
struct ActorPose {
  uint64_t player = 0, object = 0;
  std::array<float,3> position = {};
  float yaw = 0, pitch = 0, roll = 0;
  std::array<float,2> head = {};
  std::vector<AnimationLayer> layers;
  bool operator==(const ActorPose&) const = default;
  };
struct Pickup {
  // item==0: an inventory removal; otherwise a world pickup. One ordered stream.
  uint64_t player = 0, actor = 0, item = 0, instance = 0;
  uint64_t amount = 0, before = 0, after = 0;
  bool operator==(const Pickup&) const = default;
  };
struct WeaponEquipment {
  uint64_t melee = 0, ranged = 0; // Instance + 1, or zero for an empty slot.
  bool operator==(const WeaponEquipment&) const = default;
  };
struct CombatState {
  std::array<int32_t,8> attributes = {};
  int32_t xp = 0, next = 0, level = 0, lp = 0;
  uint64_t target = 0, lastHit = 0;
  uint8_t weapon = 0, down = 0; // down: 0 standing, 1 unconscious, 2 dead.
  bool operator==(const CombatState&) const = default;
  };
struct CombatNpc {
  ActorPose pose;
  CombatState state;
  bool operator==(const CombatNpc&) const = default;
  };
struct PlayerState {
  std::array<int32_t,TalentCount> skills = {}, values = {};
  std::array<int32_t,3> counters = {}; // REAL_STRENGTH, REAL_DEXTERITY, REAL_MANA_MAX.
  uint64_t heldItem = 0; // Instance + 1, or zero; presentation only.
  std::string heldSlot;
  bool operator==(const PlayerState&) const = default;
  };
struct Poses {
  uint64_t world = 0, tick = 0, appliedInput = 0;
  std::array<ActorPose,2> actors;
  std::array<WeaponEquipment,2> equipment;
  std::vector<Pickup> pickups; // Ordered, atomic transfers at this revision.
  std::array<CombatState,2> progression;
  std::vector<CombatNpc> npcs; // Bounded updates to NPCs in the initial snapshot.
  Bytes story; // Optional authoritative dialogue/journal/inventory projection.
  std::array<PlayerState,2> personal;
  bool operator==(const Poses&) const = default;
  };
struct PoseAck { bool operator==(const PoseAck&) const = default; };

using Payload = std::variant<Hello,Welcome,Input,StateAck,Reject,Accepted,
                             SnapshotBegin,SnapshotChunk,SnapshotEnd,SnapshotReady,Poses,PoseAck>;

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
