#include "networldsync.h"

#include <Tempest/Log>
#include <vector>

#include "world/objects/npc.h"
#include "world/world.h"
#include "netsession.h"

using namespace Tempest;

namespace {

// gives npc the network id from the host; false when it's taken by another object
bool bindId(NetEntityRegistry& ids, Npc& npc, NetEntityId id) {
  if(ids.id(npc)==id)
    return true;
  if(auto other = ids.npc(id); other!=nullptr && other!=&npc)
    return false;
  ids.remove(npc); // the host respawned the character with a new id, e.g. after loading its world
  return ids.bind(id, npc);
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
    const auto pos = npc->position();
    session.setAvatar({pid, id.value, pos.x, pos.y, pos.z, npc->rotation()});
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
  s.anim        = uint16_t(pl.lastAnim());
  s.walkMode    = uint8_t(pl.walkMode());
  s.weaponState = uint8_t(pl.weaponState());
  session.sendPlayerState(s);
  }

// puts the other players' characters where their players have them
// TODO(MP-11): interpolate between states and play the animation of the state
void applyStates(NetSession& session, World& world) {
  auto& ids = world.netEntities();
  for(auto& r:world.remotePlayers()) {
    auto* s = session.playerState(r.playerId);
    if(s==nullptr || ids.id(*r.npc)!=NetEntityId{s->entityId})
      continue; // nothing yet, or the state of a character the host has replaced since
    r.npc->setPosition(s->x, s->y, s->z);
    r.npc->setDirection(s->rotation);
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

  if(!online || world.player()==nullptr)
    return;
  if(session->isHost())
    tickHost(*session, world); else
    tickClient(*session, world);
  sendState(*session, world);
  applyStates(*session, world);
  }
