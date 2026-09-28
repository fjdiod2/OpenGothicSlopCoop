#pragma once

#include "protocol.h"
#include <memory>
#include <string>

namespace Coop {

enum class Role { Disabled, Host, Guest };
struct Endpoint {
  Role role = Role::Disabled;
  uint16_t port = 0;
  };

// Host accepts port 0 (OS-assigned). Guest accepts only literal 127.0.0.1:PORT.
Endpoint hostEndpoint(const std::string& text);
Endpoint joinEndpoint(const std::string& text);

struct NetworkStatus {
  Role role = Role::Disabled;
  std::string state = "disabled";
  std::string reason;
  uint16_t port = 0;
  uint64_t generation = 0;
  uint64_t boundPlayer = 0, bindingRevision = 0, bindings = 0;
  uint64_t accepted = 0, rejected = 0, timeouts = 0, disconnected = 0;
  uint64_t events = 0;
  size_t peers = 0;
  std::string snapshotState = "none";
  uint64_t snapshotRevision = 0, snapshotBytes = 0, snapshotTransferred = 0;
  Fingerprint snapshotDigest = {};
  uint64_t inputSent = 0, inputReceived = 0, inputApplied = 0, inputAcknowledged = 0;
  uint64_t inputDuplicates = 0, inputStale = 0, acknowledgedRevision = 0;
  uint64_t posesSent = 0, posesReceived = 0, posesApplied = 0, posesAcknowledged = 0, poseTick = 0;
  };

struct Snapshot {
  uint64_t generation = 0, revision = 0;
  Bytes data;
  };

class Session final {
  public:
    static constexpr uint64_t HandshakeTimeoutMs = 5000;
    static constexpr size_t MaxPeers = 4;
    static constexpr uint64_t SnapshotTimeoutMs = 180000;

    Session();
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    void start(Endpoint endpoint, const Hello& identity, uint64_t now);
    void tick(uint64_t now);
    // Host only, after resolving/creating its saved companion. No requested ID.
    // Repeated calls for the current generation are idempotent.
    bool bindCompanion(uint64_t generation);
    bool sendSnapshot(uint64_t generation, uint64_t revision, Bytes data);
    std::optional<Snapshot> takeSnapshot();
    bool snapshotLoaded(uint64_t generation, uint64_t revision);
    bool sendInput(Input input);
    std::optional<Message> takeInput();
    bool inputApplied(uint64_t generation, uint64_t sequence, uint64_t revision);
    bool sendPoses(uint64_t generation, uint64_t revision, const Poses& poses);
    std::optional<Message> takePoses();
    bool posesApplied(uint64_t generation, uint64_t revision);
    // Discard connection authority on world replacement; retain the listener.
    void invalidate(const std::string& reason);
    const NetworkStatus& status() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl;
  };

}
