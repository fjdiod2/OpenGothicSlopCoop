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
  };

class Session final {
  public:
    static constexpr uint64_t HandshakeTimeoutMs = 5000;
    static constexpr size_t MaxPeers = 4;

    Session();
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    void start(Endpoint endpoint, const Hello& identity, uint64_t now);
    void tick(uint64_t now);
    // Host only, after resolving/creating its saved companion. No requested ID.
    // Repeated calls for the current generation are idempotent.
    bool bindCompanion(uint64_t generation);
    // Discard connection authority on world replacement; retain the listener.
    void invalidate(const std::string& reason);
    const NetworkStatus& status() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl;
  };

}
