#include "networldsync.h"

#include <Tempest/Application>
#include <Tempest/Log>
#include <algorithm>
#include <cmath>
#include <map>
#include <optional>
#include <vector>

#include "game/gamescript.h"
#include "graphics/mesh/animationsolver.h"
#include "game/inventory.h"
#include "world/objects/interactive.h"
#include "world/objects/item.h"
#include "world/objects/npc.h"
#include "world/world.h"
#include "netsession.h"

using namespace Tempest;

namespace {

// true, if symbol is an instance of the scripts of class cls (C_ITEM, C_NPC), which the one sent by the other
// player or the host should be; both sides run the same scripts, so their symbol numbers are the same
bool isInstanceOf(World& world, uint32_t symbol, std::string_view cls) {
  auto& sc  = world.script();
  auto* sym = sc.findSymbol(symbol);
  if(sym==nullptr || sym->type()!=zenkit::DaedalusDataType::INSTANCE || sym->address()==0)
    return false;
  const zenkit::DaedalusSymbol* base = sym;
  while(base!=nullptr && base->parent()!=uint32_t(-1))
    base = sc.findSymbol(base->parent());
  return base!=nullptr && base!=sym && base->name()==cls;
  }

bool isItemInstance(World& world, uint32_t symbol) {
  return isInstanceOf(world, symbol, "C_ITEM");
  }

// gives npc the network id from the host; false when it's taken by another object
bool bindId(NetEntityRegistry& ids, Npc& npc, NetEntityId id) {
  if(ids.id(npc)==id)
    return true;
  if(auto other = ids.npc(id); other!=nullptr && other!=&npc)
    return false;
  ids.remove(npc); // the host respawned the character with a new id, e.g. after loading its world
  return ids.bind(id, npc);
  }

// host: true when the character of player pid has been dead for RespawnDelayMs
bool isRespawnDue(uint32_t pid, const Npc& npc) {
  static std::map<uint32_t,uint64_t> deadSince;
  const uint64_t now = Application::tickCount();
  if(!npc.isDead()) {
    deadSince.erase(pid);
    return false;
    }
  auto it = deadSince.find(pid);
  if(it==deadSince.end()) {
    deadSince[pid] = now;
    return false;
    }
  if(now-it->second<NetWorldSync::RespawnDelayMs)
    return false;
  deadSince.erase(it);
  return true;
  }

void tickHost(NetSession& session, World& world) {
  auto& ids = world.netEntities();
  for(auto& [pid,name]:session.playerList()) {
    Npc* npc = pid==session.playerId() ? world.player() : world.remotePlayer(pid);
    if(npc==nullptr) {
      auto& start = world.startPoint();
      npc = world.addRemotePlayer(pid, name, start.groundPos, 0);
      if(npc==nullptr)
        continue;
      npc->setDirection(start.direction());
      npc->updateTransform();
      Log::i("multiplayer: ", name, " entered the world");
      }
    NetEntityId id = ids.id(*npc);
    if(!id)
      id = ids.add(*npc);
    uint8_t flags = 0;
    if(isRespawnDue(pid, *npc)) {
      // back at the start point as a new entity: what is still under way for the dead one
      // (its states, attacks) no longer applies to it
      auto& start = world.startPoint();
      npc->netRespawn(start.groundPos, npc->rotation());
      npc->setDirection(start.direction());
      npc->updateTransform();
      ids.remove(*npc);
      id    = ids.add(*npc);
      flags = NetSession::Avatar::Respawn;
      Log::i("multiplayer: ", name, " is back to life");
      }
    const auto pos = npc->position();
    session.setAvatar({pid, id.value, pos.x, pos.y, pos.z, npc->rotation(), flags});
    }
  }

void tickClient(NetSession& session, World& world) {
  auto& ids = world.netEntities();
  for(auto& [pid,name]:session.playerList()) {
    auto* a = session.avatar(pid);
    if(a==nullptr)
      continue; // not spawned by the host yet
    const NetEntityId id{a->entityId};

    if(pid==session.playerId()) {
      if(!bindId(ids, *world.player(), id))
        Log::e("multiplayer: network id ", id.value, " of the local hero is taken");
      continue;
      }

    Npc* npc = world.remotePlayer(pid);
    if(npc==nullptr) {
      npc = world.addRemotePlayer(pid, name, Vec3(a->x, a->y, a->z), a->rotation);
      if(npc==nullptr)
        continue;
      Log::i("multiplayer: ", name, " entered the world");
      }
    if(!bindId(ids, *npc, id))
      Log::e("multiplayer: network id ", id.value, " of ", name, " is taken");
    }

  // characters the host has brought back to life, the local hero too
  for(auto& a:session.takeRespawns()) {
    Npc* npc = a.playerId==session.playerId() ? world.player() : world.remotePlayer(a.playerId);
    if(npc==nullptr || ids.id(*npc)!=NetEntityId{a.entityId})
      continue; // not in this world, or respawned once more since
    npc->netRespawn(Vec3(a.x, a.y, a.z), a.rotation);
    }
  }

// the matrix of an item on the ground, sent as position and axes
Matrix4x4 itemMatrix(float x, float y, float z, const NetProtocol::ItemAxes& axes) {
  Matrix4x4 m;
  m.identity();
  for(int c=0; c<3; ++c)
    for(int r=0; r<3; ++r)
      m.set(c, r, axes[size_t(c*3+r)]);
  m.set(3, 0, x);
  m.set(3, 1, y);
  m.set(3, 2, z);
  return m;
  }

// host: the npcs and the items on the ground of its world, for the clients to have them too (MP-19, MP-22)
void sendEntities(NetSession& session, World& world) {
  auto& ids = world.netEntities();
  std::vector<NetSession::Entity> list;
  list.reserve(world.npcCount() + world.itemCount());
  for(uint32_t i=0; i<world.npcCount(); ++i) {
    Npc& npc = *world.npcById(i);
    const NetEntityId id = ids.id(npc);
    if(!id || npc.isNetPlayer())
      continue; // the players' characters are spawned by PlayerSpawn
    const auto pos = npc.position();
    NetSession::Entity e;
    e.entityId = id.value;
    e.kind     = NetProtocol::EntityKind::Npc;
    e.instance = npc.instanceSymbol();
    e.x        = pos.x;
    e.y        = pos.y;
    e.z        = pos.z;
    e.rotation = npc.rotation();
    e.hp       = std::max(npc.attribute(ATR_HITPOINTS), 0);
    if(npc.isDead())
      e.flags = NetSession::Entity::Dead;
    else if(npc.isUnconscious())
      e.flags = NetSession::Entity::Unconscious;
    list.push_back(e);
    }
  for(uint32_t i=0; i<world.itemCount(); ++i) {
    Item& it = *world.itmById(i);
    const NetEntityId id = ids.id(it);
    if(!id)
      continue;
    // where it lies now: a newcomer gets it there, the others let it fall on their own
    auto& m = it.transform();
    NetSession::Entity e;
    e.entityId = id.value;
    e.kind     = NetProtocol::EntityKind::Item;
    e.instance = uint32_t(it.clsId());
    e.x        = m.at(3,0);
    e.y        = m.at(3,1);
    e.z        = m.at(3,2);
    e.count    = uint32_t(std::clamp<size_t>(it.count(), 1, NetProtocol::MaxItemCount));
    for(int c=0; c<3; ++c)
      for(int r=0; r<3; ++r)
        e.axes[size_t(c*3+r)] = m.at(c,r);
    if(it.isDynamic())
      e.flags = NetSession::Entity::Dynamic;
    list.push_back(e);
    }
  session.setEntities(std::move(list));
  }

// client: has the items on the ground the host has spawned, and no others (World::addItem refuses on a client);
// the ones the local hero is taking are held aside until the host answers (MP-22)
void receiveGroundItems(NetSession& session, World& world) {
  auto& ids  = world.netEntities();
  auto& list = session.entities();

  std::vector<Item*> gone;
  for(uint32_t i=0; i<world.itemCount(); ++i) {
    Item& it = *world.itmById(i);
    const NetEntityId id = ids.id(it);
    if(!id)
      continue;
    auto e = list.find(id.value);
    if(e==list.end() || e->second.kind!=NetProtocol::EntityKind::Item || e->second.instance!=it.clsId())
      gone.push_back(&it);
    }
  for(auto* it:gone)
    world.removeItem(*it);

  for(auto& [eid,e]:list) {
    const NetEntityId id{eid};
    if(e.kind!=NetProtocol::EntityKind::Item || ids.item(id)!=nullptr || world.isNetTake(eid))
      continue;
    if(!isItemInstance(world, e.instance)) {
      Log::e("multiplayer: unknown item instance ", e.instance, " of entity ", eid);
      continue;
      }
    Item* it = world.addNetItem(e.instance, itemMatrix(e.x, e.y, e.z, e.axes), e.count,
                                (e.flags & NetSession::Entity::Dynamic)!=0);
    if(it==nullptr)
      continue;
    if(!ids.bind(id, *it)) {
      Log::e("multiplayer: network id ", eid, " of item ", it->displayName(), " is taken");
      world.removeItem(*it);
      }
    }
  }

// the contents of a container, as MobState has them: one entry per instance, sorted by instance
std::vector<NetProtocol::MobItem> mobItems(Interactive& mob) {
  std::vector<NetProtocol::MobItem> ret;
  for(auto& [cls,count]:mob.inventory().contents()) {
    if(count==0 || ret.size()>=NetProtocol::MaxInventoryItems)
      continue;
    ret.push_back({uint32_t(cls), uint32_t(std::min<size_t>(count, NetProtocol::MaxInventoryCount))});
    }
  return ret;
  }

// the mob as it is in this world, as MobState has it
World::NetMob mobNow(Interactive& mob) {
  World::NetMob m;
  m.state    = std::clamp(mob.stateId(), NetProtocol::NoMobState, NetProtocol::MaxMobState);
  m.flags    = mob.isCracked() ? NetProtocol::MobState::Cracked : 0;
  m.triggers = mob.useTriggerCount();
  if(mob.isContainer())
    m.items = mobItems(mob);
  return m;
  }

// fills the mobs of world.netMobs() as they are now, when the world is new; false when it had them already
bool initMobs(World& world) {
  auto& cache = world.netMobs();
  const uint32_t count = world.mobsiCount();
  if(cache.size()==count)
    return false;
  cache.clear();
  cache.reserve(count);
  for(uint32_t i=0; i<count; ++i)
    cache.push_back(mobNow(*world.mobsiById(i)));
  world.netMobsDeferred().clear();
  return true;
  }

NetProtocol::MobTrigger toMobTrigger(TriggerEvent::Type t) {
  return t==TriggerEvent::T_Untrigger ? NetProtocol::MobTrigger::Untrigger : NetProtocol::MobTrigger::Trigger;
  }

TriggerEvent::Type fromMobTrigger(NetProtocol::MobTrigger t) {
  return t==NetProtocol::MobTrigger::Untrigger ? TriggerEvent::T_Untrigger : TriggerEvent::T_Trigger;
  }

// host: the mobs of its world that have changed since the last frame, for the clients (MP-24). The world as loaded is
// what the clients' worlds have too; only changes are sent. A container's contents are looked at while somebody uses it,
// when it changes state, and when another player has taken or put something
void sendMobs(NetSession& session, World& world) {
  if(initMobs(world))
    return;
  auto& cache = world.netMobs();
  for(uint32_t i=0; i<cache.size(); ++i) {
    Interactive& mob = *world.mobsiById(i);
    auto&        c   = cache[i];
    const int32_t  state    = std::clamp(mob.stateId(), NetProtocol::NoMobState, NetProtocol::MaxMobState);
    const uint8_t  flags    = mob.isCracked() ? NetProtocol::MobState::Cracked : 0;
    const uint32_t triggers = mob.useTriggerCount();
    const bool     changed  = state!=c.state || flags!=c.flags || triggers!=c.triggers;
    bool           content  = false;
    std::vector<NetProtocol::MobItem> items;
    if(mob.isContainer() && (changed || c.by!=0 || mob.isInUse())) {
      items   = mobItems(mob);
      content = items!=c.items;
      }
    if(!changed && !content) {
      c.by = 0;
      continue;
      }
    NetSession::MobState m;
    m.mob   = i;
    m.state = state;
    m.flags = flags;
    m.by    = c.by;
    if(triggers!=c.triggers)
      m.trigger = toMobTrigger(mob.lastUseTrigger());
    if(mob.isContainer())
      m.items = items;
    c.state    = state;
    c.flags    = flags;
    c.triggers = triggers;
    c.items    = std::move(items);
    c.by       = 0;
    session.setMob(m);
    }
  }

// host: the character of another player is near enough to the mob it has used, cm (the host sees it a little late)
bool isNearMob(const Npc& npc, const Interactive& mob) {
  const float reach = NetWorldSync::MobReach;
  return (npc.position()-mob.position()).quadLength()<=reach*reach;
  }

// host: what the other players' characters did with the mobs of their worlds is done to the mobs of this one, and sent
// on to everybody by sendMobs
void receivePlayerMobs(NetSession& session, World& world) {
  using M = NetProtocol::MobMove;
  auto& ids   = world.netEntities();
  auto& cache = world.netMobs();
  for(auto& e:session.takePlayerMobs()) {
    Npc*         npc = world.remotePlayer(e.playerId);
    Interactive* mob = world.mobsiById(e.mob);
    bool         ok  = npc!=nullptr && ids.id(*npc)==NetEntityId{e.entityId} && mob!=nullptr;
    if(ok && !isNearMob(*npc, *mob)) {
      Log::i("multiplayer: ", npc->displayName(), " is too far away from ", mob->tag(), " to use it");
      ok = false;
      }
    if(ok && e.move!=M::State && !mob->isContainer())
      ok = false;
    if(ok && e.mob<cache.size())
      cache[e.mob].by = e.playerId;

    switch(e.move) {
      case M::State:
        if(!ok || mob->isInUse())
          break; // somebody of this world is using it: this world's use goes on, the player's world follows it later
        mob->netSetState(e.state);
        if(e.flags & NetProtocol::MobState::Cracked)
          mob->setAsCracked(true);
        if(e.trigger!=NetProtocol::MobTrigger::None)
          mob->netTrigger(fromMobTrigger(e.trigger)); // a gate opened by a lever opens here too
        break;
      case M::Take: {
        uint32_t granted = 0;
        if(ok) {
          const size_t have = mob->inventory().itemCount(e.instance);
          granted = uint32_t(std::min<size_t>(have, e.count));
          mob->inventory().setItemCount(e.instance, have-granted, world);
          }
        // the player puts them into its own inventory, which its copies get with its next PlayerInventory (MP-23)
        session.answerMobTake(e.playerId, {e.mob, e.instance, e.count, granted});
        break;
        }
      case M::Put:
        if(!ok)
          break;
        if(!isItemInstance(world, e.instance)) {
          Log::e("multiplayer: unknown item ", e.instance, " put into ", mob->tag(), " by ", npc->displayName());
          break;
          }
        mob->inventory().setItemCount(e.instance, mob->inventory().itemCount(e.instance)+e.count, world);
        break;
      }
    }
  }

// client: gives the mob of this world what the host's has; false while the local hero uses it (applied once it lets go)
bool applyMob(World& world, const NetSession::MobState& m) {
  Interactive* mob = world.mobsiById(m.mob);
  if(mob==nullptr)
    return true; // another world file than the host's?
  if(mob==world.player()->interactive())
    return false;
  if(mob->stateId()!=m.state)
    mob->netSetState(m.state);
  if(m.flags & NetProtocol::MobState::Cracked)
    mob->setAsCracked(true);
  if(mob->isContainer() && mobItems(*mob)!=m.items) {
    auto& inv = mob->inventory();
    for(auto& [cls,count]:inv.contents()) {
      const bool keep = std::any_of(m.items.begin(), m.items.end(), [&](const auto& i){ return i.instance==cls; });
      if(!keep)
        inv.setItemCount(cls, 0, world);
      }
    for(auto& i:m.items) {
      if(!isItemInstance(world, i.instance)) {
        Log::e("multiplayer: unknown item ", i.instance, " in ", mob->tag());
        continue;
        }
      inv.setItemCount(i.instance, i.count, world);
      }
    }
  // what this world has agreed on with the host: the local hero's changes are the ones from here on
  auto& cache = world.netMobs();
  if(m.mob<cache.size())
    cache[m.mob] = mobNow(*mob);
  return true;
  }

// client: the mobs of this world follow the host's (MP-24); the ones the local hero is using wait until it lets go
void receiveMobs(NetSession& session, World& world) {
  auto& waiting = world.netMobsDeferred();
  if(initMobs(world)) {
    // a new world: everything the host has changed since its world was loaded
    for(auto& [id,m]:session.mobs())
      if(!applyMob(world, m))
        waiting[id] = m;
    }
  for(auto& m:session.takeMobChanges()) {
    Interactive* mob = world.mobsiById(m.mob);
    if(mob!=nullptr && m.trigger!=NetProtocol::MobTrigger::None)
      mob->emitTriggerEvent(fromMobTrigger(m.trigger)); // not netTrigger: this world didn't use the mob
    if(applyMob(world, m))
      waiting.erase(m.mob); else
      waiting[m.mob] = m;
    }
  for(auto it=waiting.begin(); it!=waiting.end();) {
    if(applyMob(world, it->second))
      it = waiting.erase(it); else
      ++it;
    }
  }

// client: what the local hero does with mobs, for the host to do the same (MP-24): the state of the mob it uses or used
// last whenever it changes (also as it goes back by itself when the hero has let go), and the items it takes out of
// containers and puts into them
void sendMobUse(NetSession& session, World& world) {
  static const World*  inWorld = nullptr;
  static const Npc*    hero    = nullptr;
  static Interactive*  last    = nullptr;

  auto& pl = *world.player();
  if(inWorld!=&world || hero!=&pl) {
    inWorld = &world;
    hero    = &pl;
    last    = nullptr;
    }
  if(pl.interactive()!=nullptr)
    last = pl.interactive();

  const NetEntityId id = world.netEntities().id(pl);
  auto& cache = world.netMobs();
  if(last!=nullptr && id) {
    const uint32_t mid = world.mobsiId(last);
    if(mid<cache.size()) {
      auto&      c   = cache[mid];
      World::NetMob now; // what MobState has of it, the contents aside: those go as Take and Put
      now.state    = std::clamp(last->stateId(), NetProtocol::NoMobState, NetProtocol::MaxMobState);
      now.flags    = last->isCracked() ? NetProtocol::MobState::Cracked : 0;
      now.triggers = last->useTriggerCount();
      if(now.state!=c.state || now.flags!=c.flags || now.triggers!=c.triggers) {
        NetSession::PlayerMob e;
        e.entityId = id.value;
        e.mob      = mid;
        e.move     = NetProtocol::MobMove::State;
        e.state    = now.state;
        e.flags    = now.flags;
        if(now.triggers!=c.triggers)
          e.trigger = toMobTrigger(last->lastUseTrigger());
        session.sendMob(e);
        c.state    = now.state;
        c.flags    = now.flags;
        c.triggers = now.triggers;
        }
      }
    }

  for(auto& e:world.takeNetMobEvents()) {
    if(!id)
      continue;
    NetSession::PlayerMob msg = e;
    msg.entityId = id.value;
    session.sendMob(msg);
    }
  }

// client: the host's answers to the local hero's takes from containers; what somebody else was quicker to take is given
// back (the hero took it already, see Npc::addItem)
void receiveMobAnswers(NetSession& session, World& world) {
  auto& pl = *world.player();
  for(auto& a:session.takeMobAnswers()) {
    if(a.granted>=a.count)
      continue;
    const size_t back = std::min<size_t>(a.count-a.granted, pl.inventory().itemCount(a.instance));
    if(back>0)
      pl.delItem(a.instance, uint32_t(back));
    Log::i("multiplayer: somebody else has taken ", a.count-a.granted, " of the items out of that container first");
    }
  }

// client: has the npcs the host has spawned, and no others (World::addNpc refuses on a client)
void receiveNpcs(NetSession& session, World& world) {
  if(world.netNpcsVersion()==session.entitiesVersion())
    return;
  auto& ids  = world.netEntities();
  auto& list = session.entities();

  // gone from the host's world, or their id names another npc now
  std::vector<Npc*> gone;
  for(uint32_t i=0; i<world.npcCount(); ++i) {
    Npc& npc = *world.npcById(i);
    const NetEntityId id = ids.id(npc);
    if(!id || npc.isNetPlayer())
      continue;
    auto it = list.find(id.value);
    if(it==list.end() || it->second.kind!=NetProtocol::EntityKind::Npc || it->second.instance!=npc.instanceSymbol())
      gone.push_back(&npc);
    }
  for(auto* npc:gone)
    world.removeNpc(*npc);

  for(auto& [eid,e]:list) {
    const NetEntityId id{eid};
    if(e.kind!=NetProtocol::EntityKind::Npc || ids.npc(id)!=nullptr)
      continue; // spawned already; where it goes from there is MP-20
    if(!isInstanceOf(world, e.instance, "C_NPC")) {
      Log::e("multiplayer: unknown npc instance ", e.instance, " of entity ", eid);
      continue;
      }
    Npc* npc = world.addNetNpc(e.instance, Vec3(e.x, e.y, e.z), e.rotation);
    if(npc==nullptr)
      continue;
    if(!ids.bind(id, *npc)) {
      Log::e("multiplayer: network id ", eid, " of ", npc->displayName(), " is taken");
      world.removeNpc(*npc);
      continue;
      }
    // what the host's scripts have done to it since it was made
    auto& hnpc = npc->handle();
    hnpc.attribute[ATR_HITPOINTS] = std::clamp(e.hp, 0, std::max(hnpc.attribute[ATR_HITPOINTSMAX], 1));
    if(e.flags & NetSession::Entity::Dead)
      npc->netDown(true);
    else if(e.flags & NetSession::Entity::Unconscious)
      npc->netDown(false);
    }
  receiveGroundItems(session, world);
  world.setNetNpcsVersion(session.entitiesVersion());
  }

// host: what its npcs near the other players are doing, for the clients to play it back (MP-20)
void sendNpcStates(NetSession& session, World& world) {
  if(!session.npcStatesDue())
    return;
  // the clients' characters here: the session picks the npcs near each of them
  std::vector<Vec3> viewers;
  for(auto& r:world.remotePlayers())
    viewers.push_back(r.npc->position());
  constexpr float View = NetSession::NpcViewDistance;

  auto& ids = world.netEntities();
  std::vector<NetSession::NpcState> list;
  for(uint32_t i=0; i<world.npcCount(); ++i) {
    Npc& npc = *world.npcById(i);
    const NetEntityId id = ids.id(npc);
    if(!id || npc.isNetPlayer())
      continue;
    const auto pos  = npc.position();
    bool       near = false;
    for(auto& v:viewers)
      if((v-pos).quadLength()<=View*View)
        near = true;
    if(!near)
      continue;
    NetSession::NpcState s;
    s.entityId    = id.value;
    s.x           = pos.x;
    s.y           = pos.y;
    s.z           = pos.z;
    s.rotation    = npc.rotation();
    s.bodyState   = uint32_t(npc.bodyStateMasked());
    if((s.bodyState & BS_MAX)==BS_UNCONSCIOUS && !npc.isUnconscious())
      s.bodyState = (s.bodyState & ~uint32_t(BS_MAX)) | uint32_t(BS_STAND); // getting up, see sendState
    s.hp          = std::max(npc.attribute(ATR_HITPOINTS), 0);
    s.walkMode    = uint8_t(npc.walkMode());
    s.weaponState = uint8_t(npc.weaponState());
    npc.netAnims(s.anims, NetProtocol::MaxNpcAnims, NetProtocol::MaxAnimNameLength);
    list.push_back(std::move(s));
    }
  session.setNpcStates(list);
  }

// client: the states of the host's npcs, queued on them for the playback
void receiveNpcStates(NetSession& session, World& world) {
  auto&          ids = world.netEntities();
  auto&          net = world.netNpcs();
  const uint64_t now = Application::tickCount();
  for(auto& s:session.takeNpcStates()) {
    Npc* npc = ids.npc(NetEntityId{s.entityId});
    if(npc==nullptr || npc->isNetPlayer())
      continue; // not spawned (yet)
    auto& n = net[s.entityId];
    if(n.npc!=npc)
      n = World::NetNpc{npc}; // another npc under that id than before
    n.motion.push(s, now);
    n.lastSeen = now;
    }
  std::erase_if(net, [&](const auto& e) {
    return ids.npc(NetEntityId{e.first})!=e.second.npc;
    });
  }

void applyWeapon(Npc& npc, WeaponState want, uint32_t spell);

// client: the host's npcs move along their states, NetInterpolator::Delay behind, and play what the host's play
void applyNpcStates(World& world) {
  // the host sends an npc in view at least every NpcRefreshMs: one not heard of for longer is out of view
  constexpr uint64_t Stale = NetSession::NpcRefreshMs*2;
  auto&          ids = world.netEntities();
  const uint64_t now = Application::tickCount();
  for(auto& [eid,n]:world.netNpcs()) {
    Npc* npc = ids.npc(NetEntityId{eid});
    if(npc==nullptr || npc!=n.npc)
      continue;
    if(now-n.lastSeen>Stale) {
      if(auto last = n.motion.newest()) {
        // out of view: it stays where it was last seen, but doesn't walk on the spot
        const auto bs = BodyState(last->bodyState & BS_MAX);
        if(!npc->isDown() && (bs==BS_WALK || bs==BS_RUN || bs==BS_SPRINT || bs==BS_SNEAK))
          npc->setAnim(AnimationSolver::Idle);
        n.motion.clear();
        n.anims.clear(); // coming back into view, it takes up all animations again
        }
      continue;
      }

    NetNpcInterpolator::Sample cur;
    if(!n.motion.sample(now, cur))
      continue;
    npc->setPosition(cur.x, cur.y, cur.z);
    npc->setDirection(cur.rotation);

    // falls and gets up as the host's, when the host's Hit hasn't felled it already (MP-16)
    auto&      s   = *cur.state;
    const auto bs  = BodyState(s.bodyState & BS_MAX);
    const bool was = n.unconscious;
    n.unconscious  = bs==BS_UNCONSCIOUS;
    if(bs==BS_DEAD)
      npc->netDown(true);
    else if(n.unconscious)
      npc->netDown(false);
    else if(was)
      npc->netStandUp();
    if(npc->isDown()) {
      n.anims.clear();
      continue;
      }

    auto& hp = npc->handle().attribute[ATR_HITPOINTS];
    hp = std::clamp(s.hp, 1, std::max(npc->handle().attribute[ATR_HITPOINTSMAX], 1)); // 0 only by netDown
    npc->setWalkMode(WalkBit(s.walkMode));
    if(s.anims!=n.anims) {
      npc->netPlayAnims(n.anims, s.anims, bs);
      n.anims = s.anims;
      }
    applyWeapon(*npc, WeaponState(s.weaponState), 0); // when the animation drawing it was missed
    }
  }

// the state of the local hero, sent to the other players
void sendState(NetSession& session, World& world) {
  auto& pl = *world.player();
  const NetEntityId id = world.netEntities().id(pl);
  if(!id)
    return; // client: the host hasn't announced the hero yet
  const auto pos = pl.position();
  NetSession::PlayerState s;
  s.entityId    = id.value;
  s.x           = pos.x;
  s.y           = pos.y;
  s.z           = pos.z;
  s.rotation    = pl.rotation();
  s.bodyState   = uint32_t(pl.bodyStateMasked());
  if((s.bodyState & BS_MAX)==BS_UNCONSCIOUS && !pl.isUnconscious()) {
    // getting up: the pose stays BS_UNCONSCIOUS until the animation is over, but the character is no
    // longer down (ZS_Unconscious has ended) and can't be finished off any more
    s.bodyState = (s.bodyState & ~uint32_t(BS_MAX)) | uint32_t(BS_STAND);
    }
  s.anim        = uint16_t(pl.lastAnim());
  s.walkMode    = uint8_t(pl.walkMode());
  s.weaponState = uint8_t(pl.weaponState());
  if(auto it = pl.inventory().activeWeapon(); it!=nullptr && pl.weaponState()==WeaponState::Mage)
    s.spell = uint32_t(it->clsId());
  // using a mob, the hero plays the mob's animations: the copies play the same ones (MP-24)
  if(pl.interactive()!=nullptr)
    pl.netAnims(s.anims, NetProtocol::MaxNpcAnims, NetProtocol::MaxAnimNameLength);
  session.sendPlayerState(s);
  }

// the items of a character, as PlayerInventory has them: one entry per instance, sorted by instance
std::vector<NetProtocol::InventoryItem> inventoryItems(const Npc& npc) {
  std::map<uint32_t,NetProtocol::InventoryItem> all;
  for(auto i = npc.inventory().iterator(Inventory::T_Inventory); i.isValid(); ++i) {
    // an equipped item of which the character has more comes twice: the equipped one first
    auto& e    = all[uint32_t(i->clsId())];
    e.instance = uint32_t(i->clsId());
    e.count    = uint32_t(std::min<size_t>(e.count+i.count(), NetProtocol::MaxInventoryCount));
    if(i.isEquipped()) {
      e.equipped = true;
      const uint8_t slot = i.slot();
      if(slot>=NetProtocol::FirstSpellSlot && slot<NetProtocol::FirstSpellSlot+NetProtocol::SpellSlots)
        e.slot = slot;
      }
    }
  std::vector<NetProtocol::InventoryItem> ret;
  ret.reserve(all.size());
  for(auto& [cls,e]:all) {
    if(e.count==0 || ret.size()>=NetProtocol::MaxInventoryItems)
      continue;
    ret.push_back(e);
    }
  return ret;
  }

// the items of the local hero, for the other players' copies of it; sent when they change (MP-23)
void sendInventory(NetSession& session, World& world) {
  auto& pl = *world.player();
  const NetEntityId id = world.netEntities().id(pl);
  if(!id)
    return; // client: the host hasn't announced the hero yet
  NetSession::PlayerInventory inv;
  inv.entityId = id.value;
  inv.items    = inventoryItems(pl);
  session.sendInventory(inv);
  }

// the move of the last attack started by a character, see Npc::lastAttack and Npc::lastCast
std::optional<NetProtocol::AttackMove> attackMove(const Npc& npc) {
  using A = AnimationSolver::Anim;
  using M = NetProtocol::AttackMove;
  if(npc.lastCast()!=0)
    return npc.lastCastLevel()!=0 ? M::Cast : npc.lastCastReleased() ? M::Release : M::Invest;
  const auto a  = npc.lastAttack();
  const auto ws = npc.weaponState();
  if(ws==WeaponState::Bow || ws==WeaponState::CBow)
    return a==A::Attack ? std::optional(M::Shoot) : std::nullopt;
  switch(a) {
    case A::Attack:       return M::Swing;
    case A::AttackL:      return M::SwingLeft;
    case A::AttackR:      return M::SwingRight;
    case A::AttackBlock:  return M::Parade;
    case A::AttackFinish: return M::Finish;
    default:              return std::nullopt;
    }
  }

// the attacks the local hero has started since the last frame; one per frame at most (the last one)
void sendAttacks(NetSession& session, World& world) {
  static const Npc* hero = nullptr;
  static uint32_t   sent = 0;

  auto&          pl    = *world.player();
  const uint32_t count = pl.attackCount();
  if(hero!=&pl || count<sent) {
    hero = &pl; // another hero, e.g. in a newly loaded world: its earlier attacks are not news
    sent = count;
    return;
    }
  if(count==sent)
    return;
  sent = count;

  auto&             ids = world.netEntities();
  const NetEntityId id  = ids.id(pl);
  const auto        mv  = attackMove(pl);
  if(!id || !mv)
    return;
  NetSession::PlayerAttack a;
  a.entityId = id.value;
  a.move     = *mv;
  a.target   = pl.lastAttackTarget();
  if(a.move==NetProtocol::AttackMove::Shoot || a.move==NetProtocol::AttackMove::Cast) {
    a.dx = pl.lastShot().x;
    a.dy = pl.lastShot().y;
    a.dz = pl.lastShot().z;
    }
  if(a.move==NetProtocol::AttackMove::Invest || a.move==NetProtocol::AttackMove::Release ||
     a.move==NetProtocol::AttackMove::Cast) {
    a.spell = uint32_t(pl.lastCast());
    a.level = pl.lastCastLevel();
    }
  session.sendAttack(a);
  }

// the attacks of the other players, queued on their characters until the playback reaches them
void receiveAttacks(NetSession& session, World& world) {
  // few attacks are ever waiting: more are left from a character the host has replaced
  constexpr size_t MaxWaiting = 16;
  auto& ids = world.netEntities();
  for(auto& a:session.takeAttacks()) {
    for(auto& r:world.remotePlayers()) {
      if(r.playerId!=a.playerId || ids.id(*r.npc)!=NetEntityId{a.entityId})
        continue;
      if(r.attacks.size()>=MaxWaiting)
        r.attacks.pop_front();
      r.attacks.push_back(a);
      }
    }
  }

// what the local hero has done with items, for the other players' copies and, on a client, for the host to take the
// item from the ground or put it down (MP-22)
void sendItems(NetSession& session, World& world) {
  const NetEntityId id = world.netEntities().id(*world.player());
  for(auto& e:world.takeNetItemEvents()) {
    NetSession::PlayerItem msg = e;
    msg.entityId = id.value;
    if(!id || !session.sendItem(msg)) {
      if(e.move==NetProtocol::ItemMove::Take)
        world.takeNetTake(e.item); // nobody to ask for it: it's lost like the item a dead host would drop
      }
    }
  }

// host: the item is on the ground near the character of another player, who asks for it or has put it there
bool isInReach(const Npc& npc, const Vec3& at) {
  const float reach = NetWorldSync::ItemReach;
  return (npc.position()-at).quadLength()<=reach*reach;
  }

// host: takes the item from the ground for the character of another player, if it is still there and near it
bool hostTake(World& world, Npc& npc, const NetSession::PlayerItem& e) {
  Item* it = world.netEntities().item(NetEntityId{e.item});
  if(it==nullptr)
    return false; // somebody else was quicker
  if(!isInReach(npc, it->position())) {
    Log::i("multiplayer: ", npc.displayName(), " is too far away to take ", it->displayName());
    return false;
    }
  // the player puts it into its own inventory, which its copies get with its next PlayerInventory (MP-23)
  world.removeItem(*it);
  return true;
  }

// host: puts down what another player has dropped where the player dropped it; the clients get it as an entity
void hostDrop(World& world, Npc& npc, const NetSession::PlayerItem& e) {
  if(!isItemInstance(world, e.instance)) {
    Log::e("multiplayer: unknown item ", e.instance, " dropped by ", npc.displayName());
    return;
    }
  if(!isInReach(npc, Vec3(e.x, e.y, e.z))) {
    Log::i("multiplayer: ", npc.displayName(), " dropped an item too far away");
    return;
    }
  world.addNetItem(e.instance, itemMatrix(e.x, e.y, e.z, e.axes), e.count, true, npc.handle().symbol_index());
  }

// what the other players did with items: on the host their takes and drops change the ground, and every world
// queues the animations on the players' characters until the playback reaches them
void receiveItems(NetSession& session, World& world) {
  // few are ever waiting: more are left from a character the host has replaced
  constexpr size_t MaxWaiting = 16;
  auto& ids = world.netEntities();
  for(auto& e:session.takeItems()) {
    Npc* npc = world.remotePlayer(e.playerId);
    if(npc==nullptr || ids.id(*npc)!=NetEntityId{e.entityId}) {
      if(session.isHost() && e.move==NetProtocol::ItemMove::Take)
        session.answerTake(e.playerId, {e.item, false});
      continue;
      }
    if(session.isHost()) {
      if(e.move==NetProtocol::ItemMove::Take)
        session.answerTake(e.playerId, {e.item, hostTake(world, *npc, e)});
      else if(e.move==NetProtocol::ItemMove::Drop)
        hostDrop(world, *npc, e);
      }
    for(auto& r:world.remotePlayers()) {
      if(r.npc!=npc)
        continue;
      if(r.items.size()>=MaxWaiting)
        r.items.pop_front();
      r.items.push_back(e);
      }
    }
  }

// client: the host's answers to the local hero's takes; a granted item goes into the hero's inventory
void receiveAnswers(NetSession& session, World& world) {
  auto& pl = *world.player();
  for(auto& a:session.takeAnswers()) {
    auto it = world.takeNetTake(a.item);
    if(it==nullptr)
      continue;
    if(!a.granted) {
      // gone, or still there if the host found the hero too far away: the ground is checked again
      world.setNetNpcsVersion(uint64_t(-1));
      continue;
      }
    if(it->isTorchBurn()) {
      pl.toggleTorch(); // a burning torch is taken into the hand, like in Npc::takeItem
      continue;
      }
    pl.addItem(std::move(it));
    }
  }

// the other player's character does with an item what the player did, the item's effect (Use) aside
void replayItem(World& world, Npc& npc, const NetSession::PlayerItem& e) {
  using M = NetProtocol::ItemMove;
  switch(e.move) {
    case M::Take:
      npc.netTakeItemAnim(Vec3(e.x, e.y, e.z));
      break;
    case M::Drop:
      if(e.flags & NetSession::PlayerItem::Animated)
        npc.netDropItemAnim();
      break;
    case M::Use:
      if(!isItemInstance(world, e.instance))
        Log::e("multiplayer: unknown item ", e.instance, " used by ", npc.displayName());
      else
        npc.netUseItem(e.instance, e.hp, e.hpMax);
      break;
    }
  }

// the other player's character does what the player did; on the host its blow or arrow deals the damage,
// on a client only the host's Hit does (Npc::isNetHit)
void replayAttack(World& world, Npc& npc, const NetSession::PlayerAttack& a) {
  Npc* target = a.target!=0 ? world.netEntities().npc(NetEntityId{a.target}) : nullptr;
  if(target==&npc)
    target = nullptr;
  npc.setTarget(target);

  using M = NetProtocol::AttackMove;
  if(a.move==M::Shoot) {
    // at the target where this world has it, as the player aimed at where its world had it (the arrow
    // is aimed at the target, Npc::shootBow); the bow may not be drawn here yet, the arrow flies anyway
    if(!npc.netShoot(target, Vec3(a.dx, a.dy, a.dz)))
      Log::i("multiplayer: shot of ", npc.displayName(), " missed: no bow or crossbow equipped");
    return;
    }
  if(a.move==M::Invest || a.move==M::Release || a.move==M::Cast) {
    if(!isItemInstance(world, a.spell)) {
      Log::e("multiplayer: unknown spell ", a.spell, " of ", npc.displayName());
      return;
      }
    // like a shot: the spell is cast even if it isn't drawn here yet, only the animations wait for it
    if(a.move==M::Invest)
      npc.netInvestSpell(a.spell);
    else if(a.move==M::Release)
      npc.netReleaseSpell(a.spell);
    else if(!npc.netCastSpell(a.spell, a.level, target, Vec3(a.dx, a.dy, a.dz)))
      Log::e("multiplayer: spell ", a.spell, " of ", npc.displayName(), " is not a rune or scroll");
    return;
    }

  const auto ws = npc.weaponState();
  if(ws!=WeaponState::Fist && ws!=WeaponState::W1H && ws!=WeaponState::W2H)
    return; // the weapon isn't drawn (yet): no blow
  switch(a.move) {
    case M::Swing:
      if(ws==WeaponState::Fist)
        npc.fistShoot(); else
        npc.swingSword();
      break;
    case M::SwingLeft:
      npc.swingSwordL();
      break;
    case M::SwingRight:
      npc.swingSwordR();
      break;
    case M::Parade:
      if(ws==WeaponState::Fist)
        npc.blockFist(); else
        npc.blockSword();
      break;
    case M::Finish:
      if(!npc.finishingMove()) {
        // diagnostics: which condition of Npc::canFinish/doAttack refused it on the host
        const char* why = target==nullptr               ? "no target" :
                          !target->isUnconscious()      ? "target not unconscious" :
                          ws!=WeaponState::W1H && ws!=WeaponState::W2H ? "no melee weapon drawn" :
                                                          "out of range or animation refused";
        const float dist = target!=nullptr ? npc.fightDistanceTo(*target).length() : 0.f;
        Log::i("multiplayer: finishing move of ", npc.displayName(), " missed: ", why,
               " (weapon state ", int(ws), ", distance ", int(dist), ")");
        }
      break;
    case M::Shoot:
    case M::Invest:
    case M::Release:
    case M::Cast:
      break;
    }
  }

// host: sends the hits in its world; client: plays the host's hits back
void syncHits(NetSession& session, World& world) {
  auto local = world.takeNetHits();
  if(session.isHost()) {
    for(auto& h:local)
      session.sendHit(h);
    return;
    }
  auto& ids = world.netEntities();
  for(auto& h:session.takeHits()) {
    Npc* target = ids.npc(NetEntityId{h.target});
    if(target==nullptr)
      continue; // not in this world (yet)
    Npc* attacker = h.attacker!=0 ? ids.npc(NetEntityId{h.attacker}) : nullptr;
    target->takeNetHit(attacker, h);
    }
  }

// the host owns the clock of the world, the clients take it over
void syncTime(NetSession& session, World& world) {
  if(session.isHost()) {
    session.setWorldTime(world.time().toInt());
    return;
    }
  if(auto t = session.takeWorldTime())
    world.setTime(gtime::fromInt(*t));
  }

// turning speed is measured over this long, ms
constexpr uint64_t TurnWindow = 100;

// animations of a player's movement which are replayed on its character, with aiming a bow; the rest
// (attacks, interactions, items, ...) is replayed by its own messages or belongs to the later parts
// of the synchronization
bool isMovementAnim(uint16_t a) {
  using A = AnimationSolver::Anim;
  switch(a) {
    case A::Idle:
    case A::Move:
    case A::MoveBack:
    case A::MoveL:
    case A::MoveR:
    case A::Fall:
    case A::FallDeep:
    case A::Jump:
    case A::JumpUpLow:
    case A::JumpUpMid:
    case A::JumpUp:
    case A::JumpHang:
    case A::SlideA:
    case A::SlideB:
    case A::AimBow:
      return true;
    }
  return false;
  }

// the loops a player keeps setting every frame, see PlayerMovement::implMove;
// the others play once and are started again only when the state switches to them
bool isMovementLoop(uint16_t a) {
  using A = AnimationSolver::Anim;
  return a==A::Idle || a==A::Move || a==A::MoveBack || a==A::MoveL || a==A::MoveR;
  }

// plays the animation of the state the character is in, as PlayerMovement does for the local hero
void applyAnim(World::RemotePlayer& r, const NetSession::PlayerState& s, float turnSpeed) {
  auto& npc = *r.npc;
  npc.setWalkMode(WalkBit(s.walkMode));

  // standing still doesn't cut a blow short, like in PlayerMovement::implMove; while the player charges or
  // casts a spell (animations started by the spell, not by setAnim) its character keeps the spell's animation
  const auto bs      = BodyState(s.bodyState & BS_MAX);
  const bool blow    = s.anim==AnimationSolver::Anim::Idle && npc.isAttackAnim();
  const bool casting = bs==BS_CASTING;
  // likewise while it eats, drinks or picks something up: that animation is started by the item (MP-22)
  const bool item    = bs==BodyState(BS_ITEMINTERACT & BS_MAX) || bs==BS_TAKEITEM || bs==BS_DROPITEM;
  if(isMovementAnim(s.anim) && (s.anim!=r.anim || isMovementLoop(s.anim)) && !blow && !casting && !item)
    npc.setAnim(AnimationSolver::Anim(s.anim));
  r.anim = s.anim;

  // turning on the spot: 30 degrees per second, like PlayerMovement::setAnimRotate
  int turn = 0;
  if(s.anim==AnimationSolver::Anim::Idle && std::fabs(turnSpeed)>=30.f)
    turn = turnSpeed>0 ? -1 : 1;
  npc.setAnimRotate(turn);
  }

// items a character wears or has in a weapon slot, which Inventory::use equips rather than uses
bool isEquippable(const Item& it) {
  const auto main = ItmFlags(it.mainFlag());
  const auto flag = ItmFlags(it.handle().flags);
  return (main & (ITM_CAT_NF | ITM_CAT_FF | ITM_CAT_RUNE | ITM_CAT_ARMOR))!=0 ||
         (flag & (ITM_SHIELD | ITM_BELT | ITM_AMULET | ITM_RING))!=0;
  }

// a weapon, rune or scroll in the hand of npc is put away before it is taken or another of its kind is equipped
void putAway(Npc& npc, const Item& it) {
  auto kind = [](const Item& i) { return ItmFlags(i.mainFlag()) & (ITM_CAT_NF | ITM_CAT_FF | ITM_CAT_RUNE); };
  const Item* active = npc.inventory().activeWeapon();
  if(active==&it || (active!=nullptr && kind(*active)!=0 && kind(*active)==kind(it)))
    npc.closeWeapon(true); // drawn again by applyWeapon, as the player's state says
  }

// gives the other player's character the items its player's has (MP-23): what it lacks is made by the scripts of this
// world from the instance, what the player no longer has goes, and what the player has equipped is equipped (without
// the requirements: the player could)
void applyInventory(World& world, Npc& npc, const std::vector<NetProtocol::InventoryItem>& items) {
  std::map<size_t,size_t> have;
  for(auto i = npc.inventory().iterator(Inventory::T_Inventory); i.isValid(); ++i)
    have[i->clsId()] += i.count();
  std::map<size_t,const NetProtocol::InventoryItem*> want;
  for(auto& e:items)
    want[e.instance] = &e;

  // what goes, first: an equipped item makes room for the one the player has in its place
  for(auto& [cls,count]:have) {
    Item* it = npc.getItem(cls);
    if(it==nullptr)
      continue;
    auto w = want.find(cls);
    if(w==want.end()) {
      putAway(npc, *it);
      npc.delItem(cls, uint32_t(count));
      continue;
      }
    auto& e = *w->second;
    if(it->isEquipped() && (!e.equipped || (e.slot!=0 && it->slot()!=e.slot))) {
      putAway(npc, *it);
      npc.unequipItem(cls);
      }
    if(e.count<count)
      npc.delItem(cls, uint32_t(count-e.count));
    else if(e.count>count)
      npc.addItem(cls, e.count-count);
    }

  for(auto& e:items) {
    if(have.count(e.instance)==0) {
      if(!isItemInstance(world, e.instance)) {
        Log::e("multiplayer: unknown item ", e.instance, " in the inventory of ", npc.displayName());
        continue;
        }
      npc.addItem(e.instance, e.count);
      }
    if(!e.equipped)
      continue;
    Item* it = npc.getItem(e.instance);
    if(it==nullptr || it->isEquipped() || !isEquippable(*it))
      continue;
    putAway(npc, *it);
    npc.useItem(e.instance, e.slot!=0 ? e.slot : uint8_t(Item::NSLOT), true);
    }
  }

// draws or puts away the weapon as the other player did; a switch can take a few frames (put the old
// weapon away, then draw the new one), and waits while the character can't switch, so it is repeated
// until the character is in the state. A spell is drawn when the character has its rune or scroll
// (applyInventory)
void applyWeapon(Npc& npc, WeaponState want, uint32_t spell) {
  const auto cur   = npc.weaponState();
  const bool melee = cur==WeaponState::W1H || cur==WeaponState::W2H;
  const bool bow   = cur==WeaponState::Bow || cur==WeaponState::CBow;
  switch(want) {
    case WeaponState::NoWeapon:
      if(cur!=WeaponState::NoWeapon)
        npc.closeWeapon(false);
      break;
    case WeaponState::Fist:
      if(cur!=WeaponState::Fist)
        npc.drawWeaponFist();
      break;
    case WeaponState::W1H:
    case WeaponState::W2H:
      if(melee || npc.currentMeleeWeapon()==nullptr)
        break; // without the weapon drawWeaponMelee would draw the fists again and again
      if(cur==WeaponState::Fist)
        npc.closeWeapon(false); // drawWeaponMelee takes the fists for a melee weapon
      else
        npc.drawWeaponMelee();
      break;
    case WeaponState::Bow:
    case WeaponState::CBow:
      if(!bow && npc.currentRangedWeapon()!=nullptr)
        npc.drawWeaponBow();
      break;
    case WeaponState::Mage: {
      auto* active = npc.inventory().activeWeapon();
      auto* it     = spell!=0 ? npc.getItem(spell) : nullptr;
      if(it==nullptr || !it->isSpellOrRune())
        break;
      if(cur==WeaponState::Mage && active!=nullptr && active->clsId()==spell)
        break;
      npc.drawSpell(it->spellId()); // puts another weapon away first, over a few frames
      break;
      }
    }
  }

// the character falls as its player's did, when the host's Hit hasn't felled it already, e.g. when
// only the player's own world has hurt it (a fall, drowning, npcs of its own until MP-19); an unconscious
// one gets up again once its player's has. Death is undone only by the host's respawn.
// Returns false while the character is down.
bool applyDown(World::RemotePlayer& r, const NetSession::PlayerState& s) {
  auto&      npc  = *r.npc;
  const auto bs   = BodyState(s.bodyState & BS_MAX);
  const bool was  = r.unconscious;
  r.unconscious   = bs==BS_UNCONSCIOUS; // no longer while getting up, see sendState
  if(bs==BS_DEAD)
    npc.netDown(true);
  else if(r.unconscious)
    npc.netDown(false);
  else if(was)
    npc.netStandUp(); // not merely a state from before the host's hit felled it: the player got up
  return !npc.isDown();
  }

// moves the other players' characters along the states received from their players,
// NetInterpolator::Delay behind, and plays their animations
void applyStates(NetSession& session, World& world) {
  auto&          ids = world.netEntities();
  const uint64_t now = Application::tickCount();
  for(auto& r:world.remotePlayers()) {
    const NetEntityId id = ids.id(*r.npc);
    if(auto* s = session.playerState(r.playerId); s!=nullptr && NetEntityId{s->entityId}==id) {
      if(auto last = r.motion.newest(); last!=nullptr && last->entityId!=s->entityId) {
        r.motion.clear(); // the host has replaced the character: forget the old one's way
        r.attacks.clear();
        r.items.clear();
        }
      r.motion.push(*s, now); // the same state again is ignored
      }

    NetInterpolator::Sample cur, before;
    if(!r.motion.sample(now, cur) || NetEntityId{cur.state->entityId}!=id)
      continue; // nothing yet, or the states of a character the host has replaced since
    r.motion.sample(now-TurnWindow, before);

    float turn = std::fmod(cur.rotation-before.rotation, 360.f);
    if(turn>180.f)
      turn -= 360.f;
    if(turn<-180.f)
      turn += 360.f;

    r.npc->setPosition(cur.x, cur.y, cur.z);
    r.npc->setDirection(cur.rotation);
    // the items the player had at the moment of its movement played back now
    int64_t t = 0;
    r.motion.playbackTime(now, t);
    if(auto inv = session.takeInventory(r.playerId, t); inv && NetEntityId{inv->entityId}==id) {
      r.inventory        = std::move(inv->items);
      r.inventoryApplied = false;
      }

    if(!applyDown(r, *cur.state)) {
      r.attacks.clear(); // blows the character was about to deal before it fell
      r.items.clear();
      // it has dropped its weapons (Npc::onNoHealth); up again, it gets the items its player still has
      r.inventoryApplied = false;
      continue;
      }
    applyWeapon(*r.npc, WeaponState(cur.state->weaponState), cur.state->spell);
    // using a mob (MP-24), the character plays the animations its player's plays; the mob itself follows the host's
    auto& anims = cur.state->anims;
    if(!anims.empty() || !r.anims.empty()) {
      r.npc->netPlayAnims(r.anims, anims, BodyState(cur.state->bodyState & BS_MAX));
      r.anims = anims;
      }
    if(anims.empty())
      applyAnim(r, *cur.state, turn*1000.f/float(TurnWindow)); else
      r.anim = cur.state->anim;

    // attacks at the moment of the player's movement they were started in, with its weapon drawn
    while(!r.attacks.empty() && int64_t(r.attacks.front().time)<=t) {
      replayAttack(world, *r.npc, r.attacks.front());
      r.attacks.pop_front();
      }
    while(!r.items.empty() && int64_t(r.items.front().time)<=t) {
      replayItem(world, *r.npc, r.items.front());
      r.items.pop_front();
      }
    // after them: a player sends its items after the arrow it has shot or the item it has used up in that frame,
    // which the copy has just shot or started using too (and anyway counts are the player's again next time)
    if(r.inventory && !r.inventoryApplied) {
      applyInventory(world, *r.npc, *r.inventory);
      r.inventoryApplied = true;
      }
    }
  }

}

void NetWorldSync::tick(NetSession* session, World& world) {
  const bool online = session!=nullptr && session->state()==NetSession::State::Online;

  std::vector<uint32_t> gone;
  for(auto& r:world.remotePlayers())
    if(!online || session->playerList().count(r.playerId)==0 || r.playerId==session->playerId())
      gone.push_back(r.playerId);
  for(auto id:gone)
    world.removeRemotePlayer(id);

  if(!online || world.player()==nullptr) {
    world.takeNetHits(); // nobody to send them to
    world.takeNetItemEvents();
    world.takeNetMobEvents();
    world.clearNetTakes();
    return;
    }
  if(session->isHost()) {
    tickHost(*session, world);
    receiveItems(*session, world); // before sendEntities: the items taken go out with it
    sendEntities(*session, world);
    sendNpcStates(*session, world);
    receivePlayerMobs(*session, world); // before sendMobs: the players' changes go out with it
    sendMobs(*session, world);
    } else {
    tickClient(*session, world);
    receiveAnswers(*session, world);
    receiveNpcs(*session, world);
    receiveNpcStates(*session, world);
    applyNpcStates(world);
    receiveItems(*session, world);
    sendMobUse(*session, world); // before receiveMobs: what the host sends back isn't taken for the hero's own
    receiveMobs(*session, world);
    receiveMobAnswers(*session, world);
    }
  sendItems  (*session, world);
  sendInventory(*session, world);
  syncTime   (*session, world);
  sendState  (*session, world);
  sendAttacks(*session, world);
  receiveAttacks(*session, world);
  applyStates(*session, world);
  syncHits   (*session, world);
  }
