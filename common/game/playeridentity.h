#pragma once

#include <cstdint>

// Persistent campaign roster IDs; never derived from sockets or NPC array slots.
namespace PlayerIdentity {
inline constexpr uint64_t Host = 1;
inline constexpr uint64_t Companion = 2;
}
