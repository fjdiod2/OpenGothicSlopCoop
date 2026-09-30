#include "network/fingerprint.h"
#include <chrono>
#include <fstream>
#include <iostream>
using namespace Coop;
namespace {
#define REQUIRE(x) do { if(!(x)) throw std::runtime_error(#x); } while(false)
template<class F> void throws(F action) {
  try { action(); } catch(const std::exception&) { return; }
  throw std::runtime_error("Expected exception");
  }
struct Fixture {
  std::filesystem::path root = std::filesystem::temp_directory_path()/
    ("gothic-fingerprint-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  Fixture() {
    std::filesystem::create_directories(root/"Data");
    std::filesystem::create_directories(root/"_work");
    }
  ~Fixture() { std::filesystem::remove_all(root); }
  void write(const std::string& name,const std::string& contents) {
    std::ofstream file(root/name,std::ios::binary);
    file << contents;
    REQUIRE(bool(file));
    }
  };

void fingerprints() {
  Fixture fixture;
  fixture.write("engine","abc");
  fixture.write("Data/a.vdf","archive A");
  fixture.write("Data/b.vdf","archive B");
  fixture.write("_work/game.dat","compiled script");
  fixture.write("mod.ini","selected mod");
  const std::atomic_bool cancel = false;
  const auto& root = fixture.root;
  REQUIRE(hexFingerprint(hashFile(root/"engine",cancel))==
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  FingerprintInputs inputs{root/"Data",root/"_work",root/"mod.ini",
                           {root/"Data/a.vdf",root/"Data/b.vdf"},{"game=2","language=0"}};
  const auto original = fingerprint(inputs,cancel);
  // Independently encoded manifest fixture; identical on every supported OS.
  REQUIRE(hexFingerprint(original.gameData)=="da7ab5dc7f8445d259c573150ac9b8846b2101e05eecc921ba224f0f4945c94c");
  REQUIRE(original==fingerprint(inputs,cancel));
  REQUIRE(buildIdentity().starts_with("opengothic-coop-build-v2:"));
  REQUIRE(manifestPath("Folder\\MiXeD.DAT")=="folder/mixed.dat");
  REQUIRE(hexFingerprint(hashBytes({}))=="e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  REQUIRE(hexFingerprint(hashBytes(Bytes(1000000,'a')))=="cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
  std::filesystem::rename(root/"Data/a.vdf",root/"Data/A.VDF");
  inputs.mountedArchives[0] = root/"Data/A.VDF";
  REQUIRE(fingerprint(inputs,cancel)==original);
  std::filesystem::rename(root/"Data/A.VDF",root/"Data/a.vdf");
  inputs.mountedArchives[0] = root/"Data/a.vdf";
  // Installation directory and filesystem enumeration order are not identity.
  std::filesystem::copy(root,root.string()+"-copy",std::filesystem::copy_options::recursive);
  const std::filesystem::path copied = root.string()+"-copy";
  FingerprintInputs relocated{copied/"Data",copied/"_work",copied/"mod.ini",
                              {copied/"Data/a.vdf",copied/"Data/b.vdf"},inputs.settings};
  const auto moved = fingerprint(relocated,cancel);
  std::filesystem::remove_all(copied);
  REQUIRE(moved==original);
  const auto time = std::filesystem::last_write_time(root/"Data/a.vdf");
  fixture.write("Data/a.vdf","archive C"); // Same byte count and mtime; content must still matter.
  std::filesystem::last_write_time(root/"Data/a.vdf",time);
  REQUIRE(fingerprint(inputs,cancel).gameData!=original.gameData);
  fixture.write("Data/a.vdf","archive A");
  REQUIRE(fingerprint(inputs,cancel)==original);
  std::swap(inputs.mountedArchives[0],inputs.mountedArchives[1]);
  REQUIRE(fingerprint(inputs,cancel).gameData!=original.gameData);
  std::swap(inputs.mountedArchives[0],inputs.mountedArchives[1]);
  inputs.settings[1] = "language=1";
  REQUIRE(fingerprint(inputs,cancel).gameData!=original.gameData);
  inputs.settings[1] = "language=0";
  fixture.write("mod.ini","different mod");
  REQUIRE(fingerprint(inputs,cancel).gameData!=original.gameData);
  fixture.write("mod.ini","selected mod");
  fixture.write("_work/game.dat","modified script");
  REQUIRE(fingerprint(inputs,cancel).gameData!=original.gameData);
  fixture.write("_work/game.dat","compiled script");
  fixture.write("engine","abd");
  const auto changed = fingerprint(inputs,cancel);
  REQUIRE(changed.engine==original.engine && changed.gameData==original.gameData);
  throws([&] { const std::atomic_bool stop=true; fingerprint(inputs,stop); });
  std::error_code symlinkError;
  std::filesystem::create_symlink(root/"engine",root/"Data/symlink",symlinkError);
  if(!symlinkError) throws([&] { fingerprint(inputs,cancel); });
  std::cout << "manifest-v2 " << hexFingerprint(original.gameData) << '\n';
  }
}
int main() {
  try { fingerprints(); return 0; }
  catch(const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
  }
