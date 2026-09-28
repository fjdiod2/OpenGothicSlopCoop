#pragma once

#include <cstdint>
#include <map>
#include <span>
#include <unordered_map>

namespace Coop {

enum class ObjectKind : uint8_t { Npc = 1, Item = 2 };

// A world namespace plus a serial identifies a world object. Player roster IDs
// and checkpoint array indexes are separate. Removed serials are never recycled.
class ObjectRegistry final {
  public:
    static constexpr uint32_t MaxRecords = 1'000'000;
    struct Record {
      uint64_t id = 0;
      ObjectKind kind = ObjectKind::Npc;
      void* object = nullptr; // Null is a retained tombstone, not an unknown ID.
      };

    ObjectRegistry();
    explicit ObjectRegistry(uint64_t worldNamespace);
    uint64_t worldNamespace() const { return space; }
    uint64_t nextId() const { return uint64_t(records.size())+1; }
    const std::map<uint64_t,Record>& entries() const { return records; }

    uint64_t add(ObjectKind kind, void* object);
    bool remove(ObjectKind kind, const void* object);
    uint64_t id(ObjectKind kind, const void* object) const;
    void* resolve(uint64_t worldNamespace, uint64_t id, ObjectKind kind) const;
    const Record* find(uint64_t id) const;
    // Check all IDs/types/object aliases before replacing the registry. Callers
    // resolve checkpoint-local indexes first; no pointers are written to disk.
    void restore(uint64_t worldNamespace, std::span<const Record> saved);

  private:
    uint64_t space = 0;
    std::map<uint64_t,Record> records;
    std::unordered_map<const void*,uint64_t> reverse;
  };

}
