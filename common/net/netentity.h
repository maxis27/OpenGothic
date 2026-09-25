#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <unordered_map>

class Npc;
class Item;

// Network id of an object replicated between host and clients.
// The host hands out the ids, clients take them over from the host's messages;
// the same object has the same id on every machine for as long as it is in the world.
struct NetEntityId final {
  uint32_t value = 0; // 0 = no id

  explicit operator bool() const { return value!=0; }
  bool operator == (const NetEntityId& other) const { return value==other.value; }
  bool operator != (const NetEntityId& other) const { return value!=other.value; }
  };

template<>
struct std::hash<NetEntityId> {
  size_t operator()(const NetEntityId& id) const noexcept { return std::hash<uint32_t>()(id.value); }
  };

// Two-way map NetEntityId <-> Npc* / Item* of one world.
// Npcs and items share one id space. The registry never dereferences the pointers,
// the world must remove an object before it is destroyed or leaves the world.
class NetEntityRegistry final {
  public:
    enum class Kind : uint8_t {
      None = 0,
      Npc  = 1,
      Item = 2,
      };

    // Host side: gives the object the next free id, or returns the id it already has.
    NetEntityId add(Npc&  npc);
    NetEntityId add(Item& itm);

    // Client side: attaches an id received from the host.
    // Refused when the id is 0, already taken by another object, or the object has another id.
    bool        bind(NetEntityId id, Npc&  npc);
    bool        bind(NetEntityId id, Item& itm);

    void        remove(const Npc&  npc);
    void        remove(const Item& itm);
    void        remove(NetEntityId id);
    void        clear();

    NetEntityId id(const Npc&  npc) const;
    NetEntityId id(const Item& itm) const;
    Npc*        npc (NetEntityId id) const;
    Item*       item(NetEntityId id) const;
    Kind        kind(NetEntityId id) const;

    size_t      size() const { return byId.size(); }

    // When set, the world registers every npc and item it creates (host of a session).
    bool        autoAssign() const { return autoAssignIds; }
    void        setAutoAssign(bool a) { autoAssignIds = a; }

  private:
    struct Entry final {
      Kind  kind = Kind::None;
      void* ptr  = nullptr;
      };

    NetEntityId allocate(Kind k, void* ptr);
    bool        bind(NetEntityId id, Kind k, void* ptr);
    void        remove(const void* ptr);
    NetEntityId find(const void* ptr) const;
    void*       find(NetEntityId id, Kind k) const;

    std::unordered_map<NetEntityId,Entry>       byId;
    std::unordered_map<const void*,NetEntityId> byPtr;
    uint32_t                                    nextId        = 1;
    bool                                        autoAssignIds = false;
  };
