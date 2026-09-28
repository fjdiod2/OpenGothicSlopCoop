#include "fingerprint.h"

#include <algorithm>
#include <fstream>
#include <stdexcept>

#ifdef __APPLE__
#include <CommonCrypto/CommonDigest.h>
#include <mach-o/dyld.h>
#endif

namespace Coop {
namespace {
#ifdef __APPLE__
void cancelled(const std::atomic_bool& cancel) {
  if(cancel.load()) throw std::runtime_error("Fingerprint cancelled");
  }

struct Hash {
  CC_SHA256_CTX state;
  Hash() { CC_SHA256_Init(&state); }
  void bytes(std::span<const uint8_t> data) {
    // Callers supply at most a 1 MiB file block or a bounded manifest field.
    CC_SHA256_Update(&state,data.data(),CC_LONG(data.size()));
    }
  void number(uint64_t value) {
    std::array<uint8_t,8> encoded;
    for(size_t i=0; i<8; ++i) encoded[7-i] = uint8_t(value>>(8*i));
    bytes(encoded);
    }
  void text(const std::string& text) {
    if(text.size()>4096) throw std::runtime_error("Fingerprint field too long");
    number(text.size());
    bytes({reinterpret_cast<const uint8_t*>(text.data()),text.size()});
    }
  Fingerprint finish() {
    Fingerprint result;
    CC_SHA256_Final(result.data(),&state);
    return result;
    }
  };

struct Entry { std::string name; std::filesystem::path path; };
void appendTree(std::vector<Entry>& entries, const std::filesystem::path& root, const std::string& prefix,
                const std::atomic_bool& cancel) {
  namespace fs = std::filesystem;
  if(!fs::is_directory(root) || fs::is_symlink(root))
    throw std::runtime_error("Missing or symlinked fingerprint root: "+root.string());
  for(const auto& entry:fs::recursive_directory_iterator(root)) {
    cancelled(cancel);
    if(entry.path().filename()==".DS_Store") continue;
    if(entry.is_symlink()) throw std::runtime_error("Symlink in fingerprint data: "+entry.path().string());
    if(entry.is_directory()) continue;
    if(!entry.is_regular_file()) throw std::runtime_error("Non-regular fingerprint input");
    if(entries.size()>=100000) throw std::runtime_error("Too many fingerprint inputs");
    entries.push_back({prefix+entry.path().lexically_relative(root).generic_string(),entry.path()});
    }
  }
#endif
}

Fingerprint hashBytes(std::span<const uint8_t> bytes) {
#ifdef __APPLE__
  Hash hash;
  while(!bytes.empty()) {
    const auto chunk = bytes.first(std::min(bytes.size(),size_t(1024*1024)));
    hash.bytes(chunk);
    bytes = bytes.subspan(chunk.size());
    }
  return hash.finish();
#else
  (void)bytes;
  throw std::runtime_error("Co-op fingerprints currently require macOS CommonCrypto");
#endif
  }

Fingerprint hashFile(const std::filesystem::path& file, const std::atomic_bool& cancel) {
#ifdef __APPLE__
  namespace fs = std::filesystem;
  cancelled(cancel);
  if(!fs::is_regular_file(file) || fs::is_symlink(file))
    throw std::runtime_error("Invalid fingerprint file: "+file.string());
  const auto size = fs::file_size(file);
  const auto time = fs::last_write_time(file);
  std::ifstream input(file,std::ios::binary);
  if(!input) throw std::runtime_error("Cannot open fingerprint input: "+file.string());
  Hash hash;
  std::vector<uint8_t> buffer(1024*1024);
  uint64_t count = 0;
  while(input) {
    cancelled(cancel);
    input.read(reinterpret_cast<char*>(buffer.data()),std::streamsize(buffer.size()));
    const auto read = size_t(input.gcount());
    hash.bytes(std::span(buffer).first(read));
    count += read;
    }
  if(!input.eof() || count!=size || fs::file_size(file)!=size || fs::last_write_time(file)!=time)
    throw std::runtime_error("Fingerprint input unreadable or changed: "+file.string());
  return hash.finish();
#else
  (void)file; (void)cancel;
  throw std::runtime_error("Co-op fingerprints currently require macOS CommonCrypto");
#endif
  }

Hello fingerprint(const FingerprintInputs& inputs, const std::atomic_bool& cancel) {
#ifdef __APPLE__
  Hello result;
  result.engine = hashFile(inputs.executable,cancel);
  std::vector<Entry> entries;
  appendTree(entries,inputs.data,"Data/",cancel);
  appendTree(entries,inputs.work,"_work/",cancel);
  if(!inputs.mod.empty()) entries.push_back({"selected-mod",inputs.mod});
  std::sort(entries.begin(),entries.end(),[](const auto& a,const auto& b) { return a.name<b.name; });
  Hash hash;
  hash.text("OpenGothic-data-manifest-v1");
  hash.number(inputs.settings.size());
  for(auto& setting:inputs.settings) hash.text(setting);
  hash.number(inputs.mountedArchives.size());
  for(auto& archive:inputs.mountedArchives)
    hash.text(archive.lexically_relative(inputs.data).generic_string());
  hash.number(entries.size());
  for(auto& entry:entries) {
    hash.text(entry.name);
    const auto digest = hashFile(entry.path,cancel);
    hash.bytes(digest);
    }
  result.gameData = hash.finish();
  return result;
#else
  (void)inputs; (void)cancel;
  throw std::runtime_error("Co-op fingerprints currently require macOS CommonCrypto");
#endif
  }

std::string hexFingerprint(const Fingerprint& value) {
  constexpr char hex[] = "0123456789abcdef";
  std::string result;
  result.reserve(64);
  for(auto byte:value) { result.push_back(hex[byte>>4]); result.push_back(hex[byte&15]); }
  return result;
  }

std::filesystem::path executablePath() {
#ifdef __APPLE__
  uint32_t size = 0;
  _NSGetExecutablePath(nullptr,&size);
  std::vector<char> path(size);
  if(_NSGetExecutablePath(path.data(),&size)!=0) throw std::runtime_error("Cannot locate engine executable");
  return std::filesystem::canonical(path.data());
#else
  throw std::runtime_error("Co-op executable fingerprint currently requires macOS");
#endif
  }

}
