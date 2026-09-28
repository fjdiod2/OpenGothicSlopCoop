#pragma once

#include "protocol.h"
#include <atomic>
#include <filesystem>
#include <string>

namespace Coop {

struct FingerprintInputs {
  std::filesystem::path executable, data, work, mod;
  std::vector<std::filesystem::path> mountedArchives;
  std::vector<std::string> settings;
  };

// SHA-256 through macOS CommonCrypto; no timestamp/size-only compatibility cache.
Fingerprint hashFile(const std::filesystem::path& file, const std::atomic_bool& cancel);
Hello fingerprint(const FingerprintInputs& inputs, const std::atomic_bool& cancel);
std::string hexFingerprint(const Fingerprint& value);
std::filesystem::path executablePath();

}
