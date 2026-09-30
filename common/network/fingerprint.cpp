#include "fingerprint.h"

#include <algorithm>
#include <fstream>
#include <stdexcept>
#include <memory>
#include <openssl/evp.h>
#include "coop_build.h"


namespace Coop {
namespace {
void cancelled(const std::atomic_bool& cancel) {
  if(cancel.load()) throw std::runtime_error("Fingerprint cancelled");
  }

struct Hash {
  std::unique_ptr<EVP_MD_CTX,decltype(&EVP_MD_CTX_free)> state{EVP_MD_CTX_new(),EVP_MD_CTX_free};
  Hash() {
    if(!state || EVP_DigestInit_ex(state.get(),EVP_sha256(),nullptr)!=1)
      throw std::runtime_error("Cannot initialize SHA-256");
    }
  void bytes(std::span<const uint8_t> data) {
    // Callers supply at most a 1 MiB file block or a bounded manifest field.
    if(EVP_DigestUpdate(state.get(),data.data(),data.size())!=1)
      throw std::runtime_error("SHA-256 update failed");
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
    unsigned size = 0;
    if(EVP_DigestFinal_ex(state.get(),result.data(),&size)!=1 || size!=result.size())
      throw std::runtime_error("SHA-256 finalization failed");
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
    entries.push_back({prefix+manifestPath(entry.path().lexically_relative(root)),entry.path()});
    }
  }
}

Fingerprint hashBytes(std::span<const uint8_t> bytes) {
  Hash hash;
  while(!bytes.empty()) {
    const auto chunk = bytes.first(std::min(bytes.size(),size_t(1024*1024)));
    hash.bytes(chunk);
    bytes = bytes.subspan(chunk.size());
    }
  return hash.finish();
  }

Fingerprint hashFile(const std::filesystem::path& file, const std::atomic_bool& cancel) {
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
  }

Hello fingerprint(const FingerprintInputs& inputs, const std::atomic_bool& cancel) {
  Hello result;
  cancelled(cancel);
  const auto build = buildIdentity();
  result.engine = hashBytes({reinterpret_cast<const uint8_t*>(build.data()),build.size()});
  std::vector<Entry> entries;
  appendTree(entries,inputs.data,"data/",cancel);
  appendTree(entries,inputs.work,"_work/",cancel);
  if(!inputs.mod.empty()) entries.push_back({"selected-mod",inputs.mod});
  std::sort(entries.begin(),entries.end(),[](const auto& a,const auto& b) { return a.name<b.name; });
  for(size_t i=1; i<entries.size(); ++i)
    if(entries[i-1].name==entries[i].name)
      throw std::runtime_error("Ambiguous case-folded game-data path: "+entries[i].name);
  Hash hash;
  hash.text("OpenGothic-data-manifest-v2");
  hash.number(inputs.settings.size());
  for(auto& setting:inputs.settings) hash.text(setting);
  hash.number(inputs.mountedArchives.size());
  for(auto& archive:inputs.mountedArchives)
    hash.text(manifestPath(archive.lexically_relative(inputs.data)));
  hash.number(entries.size());
  for(auto& entry:entries) {
    hash.text(entry.name);
    const auto digest = hashFile(entry.path,cancel);
    hash.bytes(digest);
    }
  result.gameData = hash.finish();
  return result;
  }

std::string hexFingerprint(const Fingerprint& value) {
  constexpr char hex[] = "0123456789abcdef";
  std::string result;
  result.reserve(64);
  for(auto byte:value) { result.push_back(hex[byte>>4]); result.push_back(hex[byte&15]); }
  return result;
  }

std::string buildIdentity() { return COOP_BUILD_ID; }

std::string manifestPath(const std::filesystem::path& path) {
  const auto utf8 = path.generic_u8string();
  std::string result(utf8.begin(),utf8.end());
  for(auto& c:result) {
    if(c=='\\') c='/';
    if(c>='A' && c<='Z') c=char(c-'A'+'a');
    }
  return result;
  }

}
