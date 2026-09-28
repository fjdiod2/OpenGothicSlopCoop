#pragma once

#include <cstdint>

// Provenance only: loading this record never restores socket/control authority.
struct CheckpointBoundary {
  uint64_t world = 0, tick = 0;
  uint64_t generation = 0, revision = 0, input = 0;
  };
