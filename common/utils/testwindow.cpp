#include "testwindow.h"

#include <Tempest/SystemApi>

namespace {
template<class Api>
constexpr bool supportsBackground = requires(bool enabled) { Api::setBackgroundMode(enabled); };

template<class Api>
bool configureBackground(bool enabled) {
  if constexpr(supportsBackground<Api>) {
    Api::setBackgroundMode(enabled);
    return true;
    }
  return !enabled;
  }
}

bool TestWindow::backgroundSupported() {
#if defined(__OSX__)
  return supportsBackground<Tempest::SystemApi>;
#else
  return false;
#endif
  }

bool TestWindow::configureBackground(bool enabled) {
#if defined(__OSX__)
  return ::configureBackground<Tempest::SystemApi>(enabled);
#else
  return !enabled;
#endif
  }
