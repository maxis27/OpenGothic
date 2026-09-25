#include "netentity.h"

NetEntityId NetEntityRegistry::add(Npc& npc) {
  return allocate(Kind::Npc,&npc);
  }

NetEntityId NetEntityRegistry::add(Item& itm) {
  return allocate(Kind::Item,&itm);
  }

bool NetEntityRegistry::bind(NetEntityId id, Npc& npc) {
  return bind(id,Kind::Npc,&npc);
  }

bool NetEntityRegistry::bind(NetEntityId id, Item& itm) {
  return bind(id,Kind::Item,&itm);
  }

void NetEntityRegistry::remove(const Npc& npc) {
  remove(static_cast<const void*>(&npc));
  }

void NetEntityRegistry::remove(const Item& itm) {
  remove(static_cast<const void*>(&itm));
  }

void NetEntityRegistry::remove(NetEntityId id) {
  auto it = byId.find(id);
  if(it==byId.end())
    return;
  byPtr.erase(it->second.ptr);
  byId.erase(it);
  }

void NetEntityRegistry::clear() {
  byId.clear();
  byPtr.clear();
  }

NetEntityId NetEntityRegistry::id(const Npc& npc) const {
  return find(static_cast<const void*>(&npc));
  }

NetEntityId NetEntityRegistry::id(const Item& itm) const {
  return find(static_cast<const void*>(&itm));
  }

Npc* NetEntityRegistry::npc(NetEntityId id) const {
  return static_cast<Npc*>(find(id,Kind::Npc));
  }

Item* NetEntityRegistry::item(NetEntityId id) const {
  return static_cast<Item*>(find(id,Kind::Item));
  }

NetEntityRegistry::Kind NetEntityRegistry::kind(NetEntityId id) const {
  auto it = byId.find(id);
  if(it==byId.end())
    return Kind::None;
  return it->second.kind;
  }

NetEntityId NetEntityRegistry::allocate(Kind k, void* ptr) {
  if(auto prev = find(ptr))
    return prev;
  // ids of bound (client) entries may lie ahead of the counter; skip them and never hand out 0
  NetEntityId id{nextId};
  while(!id || byId.find(id)!=byId.end())
    ++id.value;
  nextId = id.value+1;
  byId [id]  = Entry{k,ptr};
  byPtr[ptr] = id;
  return id;
  }

bool NetEntityRegistry::bind(NetEntityId id, Kind k, void* ptr) {
  if(!id)
    return false;
  auto it = byId.find(id);
  if(it!=byId.end())
    return it->second.ptr==ptr && it->second.kind==k;
  if(find(ptr))
    return false;
  byId [id]  = Entry{k,ptr};
  byPtr[ptr] = id;
  return true;
  }

void NetEntityRegistry::remove(const void* ptr) {
  auto it = byPtr.find(ptr);
  if(it==byPtr.end())
    return;
  byId.erase(it->second);
  byPtr.erase(it);
  }

NetEntityId NetEntityRegistry::find(const void* ptr) const {
  auto it = byPtr.find(ptr);
  if(it==byPtr.end())
    return NetEntityId{};
  return it->second;
  }

void* NetEntityRegistry::find(NetEntityId id, Kind k) const {
  auto it = byId.find(id);
  if(it==byId.end() || it->second.kind!=k)
    return nullptr;
  return it->second.ptr;
  }
