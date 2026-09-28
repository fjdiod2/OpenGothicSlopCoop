#include "objectregistry.h"

#include <random>
#include <stdexcept>

namespace Coop {
namespace {
bool validKind(ObjectKind kind) { return kind==ObjectKind::Npc || kind==ObjectKind::Item; }

uint64_t newNamespace() {
  std::random_device random;
  uint64_t value = 0;
  while(value==0) value = (uint64_t(random())<<32) | uint64_t(random());
  return value;
  }
}

ObjectRegistry::ObjectRegistry():ObjectRegistry(newNamespace()) {}

ObjectRegistry::ObjectRegistry(uint64_t worldNamespace):space(worldNamespace) {
  if(space==0) throw std::runtime_error("Invalid object namespace");
  }

uint64_t ObjectRegistry::add(ObjectKind kind, void* object) {
  if(!validKind(kind) || object==nullptr) throw std::runtime_error("Invalid world object");
  if(auto it = reverse.find(object); it!=reverse.end()) {
    if(records.at(it->second).kind!=kind) throw std::runtime_error("World object kind changed");
    return it->second;
    }
  if(records.size()>=MaxRecords) throw std::runtime_error("World object identity limit reached");
  const uint64_t serial = nextId();
  records.emplace(serial,Record{serial,kind,object});
  try { reverse.emplace(object,serial); }
  catch(...) { records.erase(serial); throw; }
  return serial;
  }

bool ObjectRegistry::remove(ObjectKind kind, const void* object) {
  auto it = reverse.find(object);
  if(it==reverse.end()) return false;
  auto& record = records.at(it->second);
  if(record.kind!=kind) throw std::runtime_error("World object kind changed");
  record.object = nullptr;
  reverse.erase(it);
  return true;
  }

uint64_t ObjectRegistry::id(ObjectKind kind, const void* object) const {
  const auto it = reverse.find(object);
  if(it==reverse.end() || records.at(it->second).kind!=kind) return 0;
  return it->second;
  }

const ObjectRegistry::Record* ObjectRegistry::find(uint64_t serial) const {
  const auto it = records.find(serial);
  return it==records.end() ? nullptr : &it->second;
  }

void* ObjectRegistry::resolve(uint64_t worldNamespace, uint64_t serial, ObjectKind kind) const {
  if(space!=worldNamespace) return nullptr;
  const auto record = find(serial);
  return record && record->kind==kind ? record->object : nullptr;
  }

void ObjectRegistry::restore(uint64_t worldNamespace, std::span<const Record> saved) {
  if(saved.size()>MaxRecords) throw std::runtime_error("World object identity limit reached");
  ObjectRegistry next(worldNamespace);
  for(const auto& record:saved) {
    // All issued serials, including tombstones, must be present. Together with
    // uniqueness this range check forbids gaps and preserves the high-water mark.
    if(record.id==0 || record.id>saved.size() || !validKind(record.kind) ||
       !next.records.emplace(record.id,record).second)
      throw std::runtime_error("Invalid or duplicate world object identity");
    if(record.object!=nullptr && !next.reverse.emplace(record.object,record.id).second)
      throw std::runtime_error("Duplicate world object binding");
    }
  *this = std::move(next);
  }

}
