#include "networldsync.h"

#include <Tempest/Application>
#include <Tempest/Log>
#include <cmath>
#include <vector>

#include "graphics/mesh/animationsolver.h"
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

// animations of a player's movement which are replayed on its character; the rest (attacks,
// interactions, items, ...) belongs to the later parts of the synchronization
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

  if(isMovementAnim(s.anim) && (s.anim!=r.anim || isMovementLoop(s.anim)))
    npc.setAnim(AnimationSolver::Anim(s.anim));
  r.anim = s.anim;

  // turning on the spot: 30 degrees per second, like PlayerMovement::setAnimRotate
  int turn = 0;
  if(s.anim==AnimationSolver::Anim::Idle && std::fabs(turnSpeed)>=30.f)
    turn = turnSpeed>0 ? -1 : 1;
  npc.setAnimRotate(turn);
  }

// moves the other players' characters along the states received from their players,
// NetInterpolator::Delay behind, and plays their animations
void applyStates(NetSession& session, World& world) {
  auto&          ids = world.netEntities();
  const uint64_t now = Application::tickCount();
  for(auto& r:world.remotePlayers()) {
    const NetEntityId id = ids.id(*r.npc);
    if(auto* s = session.playerState(r.playerId); s!=nullptr && NetEntityId{s->entityId}==id) {
      if(auto last = r.motion.newest(); last!=nullptr && last->entityId!=s->entityId)
        r.motion.clear(); // the host has replaced the character: forget the old one's way
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
    applyAnim(r, *cur.state, turn*1000.f/float(TurnWindow));
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
  syncTime (*session, world);
  sendState(*session, world);
  applyStates(*session, world);
  }
