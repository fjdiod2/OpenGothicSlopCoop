#include "game/objectregistry.h"

#include <algorithm>
#include <array>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace Coop;
namespace {
#define REQUIRE(x) do { if(!(x)) throw std::runtime_error(std::string(__FILE__)+":"+std::to_string(__LINE__)+": " #x); } while(false)
template<class F> void throws(F action) {
  try { action(); } catch(const std::runtime_error&) { return; }
  throw std::runtime_error("Expected registry validation failure");
  }

void lifecycle() {
  ObjectRegistry registry(17);
  int host=0, guest=0, slot=0, other=0;
  const auto hero = registry.add(ObjectKind::Npc,&host);
  const auto companion = registry.add(ObjectKind::Npc,&guest);
  const auto plant = registry.add(ObjectKind::Item,&slot);
  REQUIRE(hero!=companion && companion!=plant);
  REQUIRE(registry.add(ObjectKind::Npc,&guest)==companion);
  REQUIRE(registry.nextId()==4);
  REQUIRE(registry.resolve(17,hero,ObjectKind::Npc)==&host);
  REQUIRE(registry.resolve(18,hero,ObjectKind::Npc)==nullptr);
  REQUIRE(registry.resolve(17,hero,ObjectKind::Item)==nullptr);
  REQUIRE(registry.resolve(17,0,ObjectKind::Npc)==nullptr);
  REQUIRE(registry.find(999)==nullptr);
  REQUIRE(registry.remove(ObjectKind::Item,&slot));
  REQUIRE(!registry.remove(ObjectKind::Item,&slot));
  REQUIRE(registry.find(plant) && registry.find(plant)->object==nullptr);
  REQUIRE(registry.id(ObjectKind::Item,&slot)==0);
  REQUIRE(registry.resolve(17,plant,ObjectKind::Item)==nullptr);
  // Allocator/address reuse must produce a new serial, including for a new kind.
  REQUIRE(registry.add(ObjectKind::Item,&slot)>plant);
  REQUIRE(registry.remove(ObjectKind::Item,&slot));
  REQUIRE(registry.add(ObjectKind::Npc,&slot)>plant+1);
  REQUIRE(registry.add(ObjectKind::Item,&other)>plant+2);
  REQUIRE(registry.id(ObjectKind::Item,&slot)==0);
  throws([&] { registry.add(ObjectKind::Item,&host); });
  throws([&] { registry.remove(ObjectKind::Item,&host); });
  throws([&] { registry.add(ObjectKind(77),&other); });
  throws([&] { registry.add(ObjectKind::Npc,nullptr); });
  REQUIRE(registry.resolve(17,hero,ObjectKind::Npc)==&host);
  }

void checkpoint() {
  ObjectRegistry original(41), loaded(99);
  std::array<int,4> before{}, after{};
  const auto host = original.add(ObjectKind::Npc,&before[0]);
  const auto companion = original.add(ObjectKind::Npc,&before[1]);
  const auto removed = original.add(ObjectKind::Item,&before[2]);
  const auto survivor = original.add(ObjectKind::Item,&before[3]);
  original.remove(ObjectKind::Item,&before[2]);
  // A checkpoint resolver binds stable serials to newly constructed objects,
  // independent of ordering, addresses and save-local array slots.
  std::vector<ObjectRegistry::Record> records{{survivor,ObjectKind::Item,&after[0]},
                                            {companion,ObjectKind::Npc,&after[3]},
                                            {removed,ObjectKind::Item,nullptr},
                                            {host,ObjectKind::Npc,&after[2]}};
  loaded.restore(original.worldNamespace(),records);
  REQUIRE(loaded.worldNamespace()==41 && loaded.nextId()==original.nextId());
  REQUIRE(loaded.resolve(41,host,ObjectKind::Npc)==&after[2]);
  REQUIRE(loaded.id(ObjectKind::Item,&after[0])==survivor);
  REQUIRE(loaded.find(removed)->object==nullptr);
  REQUIRE(loaded.add(ObjectKind::Item,&after[1])>survivor);
  // Retiring the highest serial must not lower the saved allocator watermark.
  loaded.remove(ObjectKind::Item,&after[1]);
  records.clear();
  for(const auto& [id,record]:loaded.entries()) records.push_back(record);
  loaded.restore(41,records);
  REQUIRE(loaded.add(ObjectKind::Item,&after[1])==survivor+2);
  }

void malformed() {
  int host=0, item=0;
  ObjectRegistry registry(52);
  registry.add(ObjectKind::Npc,&host);
  const std::vector<ObjectRegistry::Record> valid{{1,ObjectKind::Npc,&host},{2,ObjectKind::Item,&item}};
  for(int fault=0; fault<7; ++fault) {
    auto records = valid;
    if(fault==0) records[1].id=1;
    if(fault==1) records[1].object=&host;
    if(fault==2) records[1].id=0;
    if(fault==3) records[1].id=3;
    if(fault==4) records[1].kind=ObjectKind(3);
    if(fault==5) records[1].id=UINT64_MAX;
    throws([&] { registry.restore(fault==6 ? 0 : 19,records); });
    REQUIRE(registry.worldNamespace()==52 && registry.nextId()==2);
    REQUIRE(registry.resolve(52,1,ObjectKind::Npc)==&host);
    }
  std::vector<ObjectRegistry::Record> excessive(size_t(ObjectRegistry::MaxRecords)+1);
  throws([&] { registry.restore(19,excessive); });
  REQUIRE(registry.worldNamespace()==52 && registry.nextId()==2);
  }

void churn() {
  ObjectRegistry registry(123);
  int reusedAddress = 0;
  for(uint64_t i=1; i<=10000; ++i) {
    REQUIRE(registry.add(ObjectKind::Item,&reusedAddress)==i);
    REQUIRE(registry.remove(ObjectKind::Item,&reusedAddress));
    REQUIRE(registry.find(i)->object==nullptr);
    }
  std::vector<ObjectRegistry::Record> records;
  for(const auto& [id,record]:registry.entries()) records.push_back(record);
  std::reverse(records.begin(),records.end());
  ObjectRegistry loaded(999);
  loaded.restore(123,records);
  REQUIRE(loaded.nextId()==10001);
  REQUIRE(loaded.add(ObjectKind::Npc,&reusedAddress)==10001);
  for(uint64_t i=1; i<=10000; ++i)
    REQUIRE(loaded.resolve(123,i,ObjectKind::Item)==nullptr);
  }
}

int main(int argc, char** argv) {
  try {
    REQUIRE(argc==2);
    const std::string name=argv[1];
    if(name=="lifecycle") lifecycle();
    else if(name=="checkpoint") checkpoint();
    else if(name=="malformed") malformed();
    else if(name=="churn") churn();
    else throw std::runtime_error("Unknown object registry test");
    std::cout << "PASS: " << name << '\n';
    return 0;
    }
  catch(const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
    }
  }
