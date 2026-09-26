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

// host: the npcs of its world, for the clients to have them too (MP-19)
void sendNpcs(NetSession& session, World& world) {
  auto& ids = world.netEntities();
  std::vector<NetSession::Entity> list;
  list.reserve(world.npcCount());
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
  session.setEntities(std::move(list));
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
    if(it==list.end() || it->second.instance!=npc.instanceSymbol())
      gone.push_back(&npc);
    }
  for(auto* npc:gone)
    world.removeNpc(*npc);

  for(auto& [eid,e]:list) {
    const NetEntityId id{eid};
    if(ids.npc(id)!=nullptr)
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
  if(auto it = pl.currentMeleeWeapon())
    s.meleeWeapon = uint32_t(it->clsId());
  if(auto it = pl.currentRangedWeapon())
    s.rangedWeapon = uint32_t(it->clsId());
  if(auto it = pl.inventory().activeWeapon(); it!=nullptr && pl.weaponState()==WeaponState::Mage)
    s.spell = uint32_t(it->clsId());
  session.sendPlayerState(s);
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
  const bool blow    = s.anim==AnimationSolver::Anim::Idle && npc.isAttackAnim();
  const bool casting = BodyState(s.bodyState & BS_MAX)==BS_CASTING;
  if(isMovementAnim(s.anim) && (s.anim!=r.anim || isMovementLoop(s.anim)) && !blow && !casting)
    npc.setAnim(AnimationSolver::Anim(s.anim));
  r.anim = s.anim;

  // turning on the spot: 30 degrees per second, like PlayerMovement::setAnimRotate
  int turn = 0;
  if(s.anim==AnimationSolver::Anim::Idle && std::fabs(turnSpeed)>=30.f)
    turn = turnSpeed>0 ? -1 : 1;
  npc.setAnimRotate(turn);
  }

// gives npc the weapon of the other player in place of the one it has in the slot
void equipWeapon(World& world, Npc& npc, Item* current, uint32_t symbol, bool drawn, const char* what) {
  const size_t cur = current!=nullptr ? current->clsId() : size_t(-1);
  if(cur==symbol)
    return;
  if(drawn)
    npc.closeWeapon(true); // the weapon in hand is about to be taken away
  if(current!=nullptr)
    npc.delItem(cur, uint32_t(current->count()));
  if(symbol==0)
    return;
  if(!isItemInstance(world, symbol)) {
    Log::e("multiplayer: unknown ", what, " ", symbol, " of ", npc.displayName());
    return;
    }
  if(npc.addItem(symbol, 1)!=nullptr)
    npc.useItem(symbol, Item::NSLOT, true); // no requirements: the other player could equip it
  }

// equips the weapons the other player has on
void applyEquipment(World& world, World::RemotePlayer& r, const NetSession::PlayerState& s) {
  auto&      npc = *r.npc;
  const auto ws  = npc.weaponState();
  if(s.meleeWeapon!=r.melee) {
    equipWeapon(world, npc, npc.currentMeleeWeapon(), s.meleeWeapon,
                ws==WeaponState::W1H || ws==WeaponState::W2H, "melee weapon");
    r.melee = s.meleeWeapon;
    }
  if(s.rangedWeapon!=r.ranged) {
    equipWeapon(world, npc, npc.currentRangedWeapon(), s.rangedWeapon,
                ws==WeaponState::Bow || ws==WeaponState::CBow, "ranged weapon");
    r.ranged = s.rangedWeapon;
    }
  if(s.spell!=r.spell) {
    // the rune or scroll is given to the character to draw it; runes and scrolls it no longer holds are
    // kept (items belong to MP-22)
    if(s.spell!=0 && !isItemInstance(world, s.spell))
      Log::e("multiplayer: unknown spell ", s.spell, " of ", npc.displayName());
    else if(s.spell!=0 && npc.getItem(s.spell)==nullptr)
      npc.addItem(s.spell, 1);
    r.spell = s.spell;
    }
  }

// draws or puts away the weapon as the other player did; a switch can take a few frames (put the old
// weapon away, then draw the new one), and waits while the character can't switch, so it is repeated
// until the character is in the state. A spell is drawn when the character has its rune or scroll
// (applyEquipment)
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
    if(!applyDown(r, *cur.state)) {
      r.attacks.clear(); // blows the character was about to deal before it fell
      continue;
      }
    applyEquipment(world, r, *cur.state);
    applyWeapon(*r.npc, WeaponState(cur.state->weaponState), cur.state->spell);
    applyAnim(r, *cur.state, turn*1000.f/float(TurnWindow));

    // attacks at the moment of the player's movement they were started in, with its weapon drawn
    int64_t t = 0;
    r.motion.playbackTime(now, t);
    while(!r.attacks.empty() && int64_t(r.attacks.front().time)<=t) {
      replayAttack(world, *r.npc, r.attacks.front());
      r.attacks.pop_front();
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
    return;
    }
  if(session->isHost()) {
    tickHost(*session, world);
    sendNpcs(*session, world);
    sendNpcStates(*session, world);
    } else {
    tickClient(*session, world);
    receiveNpcs(*session, world);
    receiveNpcStates(*session, world);
    applyNpcStates(world);
    }
  syncTime   (*session, world);
  sendState  (*session, world);
  sendAttacks(*session, world);
  receiveAttacks(*session, world);
  applyStates(*session, world);
  syncHits   (*session, world);
  }
