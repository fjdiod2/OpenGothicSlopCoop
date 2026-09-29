#include "testwindow.h"
#include <Tempest/Platform>

#if defined(__OSX__)
#import <AppKit/AppKit.h>

namespace {
uint64_t activations = 0;
uint64_t keyEvents = 0;
}

void TestWindow::observe() {
  // Install before Tempest creates NSApplication/windows, so even a brief
  // startup activation is observable. These observers live with the process.
  auto center = [NSNotificationCenter defaultCenter];
  [center addObserverForName:NSApplicationDidBecomeActiveNotification object:nil queue:nil
                usingBlock:^(NSNotification*) { ++activations; }];
  [center addObserverForName:NSWindowDidBecomeKeyNotification object:nil queue:nil
                usingBlock:^(NSNotification*) { ++keyEvents; }];
  }

TestWindow::State TestWindow::state() {
  return {bool([NSApp isActive]), bool([[NSApp keyWindow] isKeyWindow]),
          activations, keyEvents, [[NSWorkspace sharedWorkspace] frontmostApplication].processIdentifier};
  }
#endif
