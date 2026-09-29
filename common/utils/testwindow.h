#pragma once

#include <cstdint>

// Native macOS focus observations for the opt-in development control channel.
namespace TestWindow {
struct State {
  bool active = false;
  bool keyWindow = false;
  uint64_t activations = 0;
  uint64_t keyEvents = 0;
  int32_t frontmostPid = 0;
  };
void observe();
State state();
}
