#include "netsession.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <utility>

using namespace NetProtocol;

namespace {

// a client gives up when it isn't welcomed by then (ENet alone would keep trying for ~30 s)
constexpr uint64_t ConnectTimeoutMs = 5000;

uint64_t nowMs() {
  using namespace std::chrono;
  return uint64_t(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
  }

// the npc has moved or does something else than when it was last sent
bool npcChanged(const NpcState& a, const NpcState& b) {
  constexpr float Move = 1.f;   // cm
  constexpr float Turn = 0.5f;  // degrees
  float turn = std::fabs(std::fmod(a.rotation-b.rotation, 360.f));
  if(turn>180.f)
    turn = 360.f-turn;
  return std::fabs(a.x-b.x)>Move || std::fabs(a.y-b.y)>Move || std::fabs(a.z-b.z)>Move || turn>Turn ||
         a.bodyState!=b.bodyState || a.hp!=b.hp || a.walkMode!=b.walkMode || a.weaponState!=b.weaponState ||
         a.anims!=b.anims;
  }

// bytes of one npc in an NpcStates packet
size_t npcStateSize(const NpcState& n) {
  size_t sz = 4*7 + 3;
  for(size_t i=0; i<n.anims.size() && i<MaxNpcAnims; ++i)
    sz += 2 + std::min(n.anims[i].size(), MaxAnimNameLength);
  return sz;
  }

// chat comes from other players: keep it on one line
std::string sanitize(std::string_view text) {
  std::string ret(text.substr(0, MaxChatLength));
  for(auto& c:ret)
    if(uint8_t(c)<0x20 || c==0x7F)
      c = ' ';
  return ret;
  }

}

NetSession::NetSession(bool server, std::string_view name)
  :server(server), name(name.substr(0, MaxNameLength)), startTime(nowMs()) {
  }

NetSession::~NetSession() {
  net.close();
  }

std::unique_ptr<NetSession> NetSession::host(uint16_t port, std::string_view name, std::string_view world) {
  std::unique_ptr<NetSession> s(new NetSession(true, name));
  if(!s->net.host(port, MaxPlayers-1))
    return nullptr;
  s->world = std::string(world.substr(0, MaxWorldLength));
  s->st    = State::Online;
  s->self  = HostPlayer;
  s->players[HostPlayer] = s->name;
  return s;
  }

std::unique_ptr<NetSession> NetSession::connect(std::string_view hostName, uint16_t port, std::string_view name) {
  std::unique_ptr<NetSession> s(new NetSession(false, name));
  s->hostPeer = s->net.connect(std::string(hostName), port);
  if(s->hostPeer==NetTransport::InvalidPeer)
    return nullptr;
  return s;
  }

std::string_view NetSession::playerName(PlayerId id) const {
  auto it = players.find(id);
  if(it==players.end())
    return {};
  return it->second;
  }

auto NetSession::avatar(PlayerId id) const -> const Avatar* {
  auto it = avatars.find(id);
  if(it==avatars.end())
    return nullptr;
  return &it->second;
  }

void NetSession::setAvatar(const Avatar& a) {
  if(!server || players.count(a.playerId)==0 || a.entityId==0)
    return;
  auto it = avatars.find(a.playerId);
  if(it!=avatars.end() && it->second.entityId==a.entityId)
    return; // already known to everyone; movement is not sent here
  avatars[a.playerId] = a;
  sendOthers(NetTransport::InvalidPeer, a);
  }

bool NetSession::sendPlayerState(const PlayerState& s) {
  const uint64_t now = nowMs();
  if(st!=State::Online || s.entityId==0 || (stateSeq!=0 && now-stateSent<StateIntervalMs))
    return false;
  PlayerState msg = s;
  msg.playerId = self;
  msg.seq      = ++stateSeq;
  msg.time     = uint32_t(now-startTime);
  stateSent    = now;
  // lost states aren't resent: the next one replaces them anyway
  if(server)
    sendOthers(NetTransport::InvalidPeer, msg, NetTransport::Unreliable); else
    send(hostPeer, msg, NetTransport::Unreliable);
  return true;
  }

auto NetSession::playerState(PlayerId id) const -> const PlayerState* {
  auto it = states.find(id);
  if(it==states.end())
    return nullptr;
  return &it->second;
  }

void NetSession::onPlayerState(PlayerId from, PlayerState s, NetTransport::PeerId peer) {
  if(from==self || players.count(from)==0)
    return;
  s.playerId = from;
  auto it = states.find(from);
  // the unreliable channel may reorder, and the host relays: keep only newer states
  if(it!=states.end() && int32_t(s.seq - it->second.seq)<=0)
    return;
  states[from] = s;
  if(server)
    sendOthers(peer, s, NetTransport::Unreliable);
  }

bool NetSession::sendAttack(const PlayerAttack& a) {
  if(st!=State::Online || a.entityId==0)
    return false;
  PlayerAttack msg = a;
  msg.playerId = self;
  msg.time     = uint32_t(nowMs()-startTime);
  if(server)
    sendOthers(NetTransport::InvalidPeer, msg); else
    send(hostPeer, msg);
  return true;
  }

auto NetSession::takeAttacks() -> std::vector<PlayerAttack> {
  return std::exchange(attacks, {});
  }

void NetSession::onAttack(PlayerId from, PlayerAttack a, NetTransport::PeerId peer) {
  if(from==self || players.count(from)==0)
    return;
  if(server) {
    // a player attacks only with its own character
    auto it = avatars.find(from);
    if(it==avatars.end() || it->second.entityId!=a.entityId)
      return;
    }
  a.playerId = from;
  if(attacks.size()<MaxPending)
    attacks.push_back(a);
  if(server)
    sendOthers(peer, a);
  }

void NetSession::sendHit(const Hit& h) {
  if(!server || st!=State::Online || h.target==0)
    return;
  sendOthers(NetTransport::InvalidPeer, h);
  }

auto NetSession::takeHits() -> std::vector<Hit> {
  return std::exchange(hits, {});
  }

auto NetSession::takeRespawns() -> std::vector<Avatar> {
  return std::exchange(respawns, {});
  }

void NetSession::setWorldTime(int64_t time) {
  if(!server || time<0)
    return;
  worldTime = time;
  const uint64_t now     = nowMs();
  const bool     jumped  = worldTimeSent && (time<*worldTimeSent || time-*worldTimeSent>=WorldTimeJump);
  if(worldTimeSent && !jumped && now-worldTimeSentAt<WorldTimeIntervalMs)
    return;
  worldTimeSent   = time;
  worldTimeSentAt = now;
  sendOthers(NetTransport::InvalidPeer, WorldTime{time});
  }

auto NetSession::takeWorldTime() -> std::optional<int64_t> {
  if(server)
    return std::nullopt;
  return std::exchange(worldTime, std::nullopt);
  }

void NetSession::setEntities(std::vector<Entity> list) {
  if(!server)
    return;
  std::sort(list.begin(), list.end(), [](const Entity& a, const Entity& b){ return a.entityId<b.entityId; });
  // both sorted by id: one walk finds the new, the replaced and the gone ones
  auto old = entityMap.begin();
  for(size_t i=0; i<list.size(); ++i) {
    const Entity& e = list[i];
    if(e.entityId==0 || e.instance==0 || (i>0 && list[i-1].entityId==e.entityId))
      continue;
    while(old!=entityMap.end() && old->first<e.entityId) {
      sendOthers(NetTransport::InvalidPeer, DespawnEntity{old->first});
      old = entityMap.erase(old);
      }
    if(old!=entityMap.end() && old->first==e.entityId) {
      const bool same = old->second.kind==e.kind && old->second.instance==e.instance;
      old->second = e;
      if(!same)
        sendOthers(NetTransport::InvalidPeer, e);
      ++old;
      continue;
      }
    entityMap.emplace_hint(old, e.entityId, e);
    sendOthers(NetTransport::InvalidPeer, e);
    }
  while(old!=entityMap.end()) {
    sendOthers(NetTransport::InvalidPeer, DespawnEntity{old->first});
    old = entityMap.erase(old);
    }
  }

bool NetSession::npcStatesDue() const {
  return server && st==State::Online && (npcSeq==0 || nowMs()-npcSentAt>=NpcStateIntervalMs);
  }

void NetSession::setNpcStates(const std::vector<NpcState>& list) {
  if(!npcStatesDue())
    return;
  const uint64_t now = nowMs();
  npcSentAt = now;
  ++npcSeq;
  const uint32_t time = uint32_t(now-startTime);

  for(auto& [peer,pid]:peers) {
    if(pid==NoPlayer)
      continue;
    // the player's character: where its player last said it is, else where the host spawned it
    float px = 0, py = 0, pz = 0;
    if(auto s = playerState(pid)) {
      px = s->x; py = s->y; pz = s->z;
      }
    else if(auto a = avatar(pid)) {
      px = a->x; py = a->y; pz = a->z;
      }
    else
      continue;

    auto&     sent = npcSent[pid];
    NpcStates pkg;
    size_t    size = 10;
    auto flush = [&]() {
      if(pkg.npcs.empty())
        return;
      pkg.seq  = npcSeq;
      pkg.time = time;
      // lost ones aren't resent as such: a change is repeated for NpcRepeatMs anyway
      send(peer, pkg, NetTransport::Unreliable);
      pkg.npcs.clear();
      size = 10;
      };

    std::unordered_map<uint32_t,NpcSent> next;
    next.reserve(sent.size());
    for(auto& n:list) {
      const float dx = n.x-px, dy = n.y-py, dz = n.z-pz;
      if(n.entityId==0 || dx*dx+dy*dy+dz*dz>NpcViewDistance*NpcViewDistance)
        continue; // out of view: sent again as a newcomer when it comes back
      NpcSent e;
      e.changed = now; // new to the client
      if(auto it = sent.find(n.entityId); it!=sent.end()) {
        e = it->second;
        if(npcChanged(e.st, n))
          e.changed = now;
        else if(now-e.changed>=NpcRepeatMs && now-e.sent<NpcRefreshMs) {
          next[n.entityId] = e; // the client has it: compared with what it has, not with the latest
          continue;
          }
        }
      e.st   = n;
      e.sent = now;
      next[n.entityId] = e;

      NpcState out = n;
      out.seq  = npcSeq;
      out.time = time;
      const size_t sz = npcStateSize(out);
      if(size+sz>NpcPacketBytes || pkg.npcs.size()>=MaxNpcStates)
        flush();
      pkg.npcs.push_back(std::move(out));
      size += sz;
      }
    flush();
    sent = std::move(next);
    }
  }

auto NetSession::takeNpcStates() -> std::vector<NpcState> {
  return std::exchange(npcStates, {});
  }

void NetSession::clearWorld() {
  players.clear();
  avatars.clear();
  states.clear();
  attacks.clear();
  hits.clear();
  respawns.clear();
  if(!entityMap.empty())
    ++entityVersion;
  entityMap.clear();
  npcSent.clear();
  npcStates.clear();
  }

void NetSession::forgetPlayer(PlayerId id) {
  players.erase(id);
  npcSent.erase(id);
  avatars.erase(id);
  states .erase(id);
  std::erase_if(attacks, [id](const PlayerAttack& a){ return a.playerId==id; });
  }

void NetSession::poll(uint32_t timeoutMs) {
  if(st==State::Closed)
    return;
  net.poll([this](const NetTransport::Event& e) {
    if(server)
      hostEvent(e); else
      clientEvent(e);
    }, timeoutMs);
  if(st==State::Connecting && nowMs()-startTime>ConnectTimeoutMs) {
    notify("Unable to connect to the host");
    st = State::Closed;
    }
  if(st==State::Closed)
    net.close(); // not from inside the callback: the transport is still dispatching there
  }

bool NetSession::sendChat(std::string_view text) {
  const std::string line = sanitize(text);
  if(st!=State::Online || line.find_first_not_of(' ')==std::string::npos)
    return false;
  if(server)
    sendOthers(NetTransport::InvalidPeer, Chat{self, line}); else
    send(hostPeer, Chat{NoPlayer, line});
  chatLine(self, line);
  return true;
  }

void NetSession::hostEvent(const NetTransport::Event& e) {
  switch(e.type) {
    case NetTransport::EventType::Connect:
      peers[e.peer] = NoPlayer;
      return;
    case NetTransport::EventType::Disconnect: {
      auto it = peers.find(e.peer);
      if(it==peers.end())
        return;
      const PlayerId id = it->second;
      peers.erase(it);
      if(id==NoPlayer)
        return;
      notify(std::string(playerName(id)) + " left the game");
      forgetPlayer(id);
      sendOthers(e.peer, PlayerLeft{id});
      return;
      }
    case NetTransport::EventType::Receive:
      break;
    }

  auto it = peers.find(e.peer);
  if(it==peers.end())
    return;
  const PlayerId from = it->second;

  auto msg = decode(e.data, e.size);
  if(!msg) {
    if(from==NoPlayer)
      reject(e.peer, Reject{RejectReason::Malformed, Version, "malformed packet"});
    return;
    }
  if(from==NoPlayer) {
    if(auto hello = std::get_if<Hello>(&*msg))
      onHello(e.peer, *hello);
    return; // nothing else before the handshake
    }
  if(auto state = std::get_if<PlayerState>(&*msg)) {
    onPlayerState(from, *state, e.peer);
    return;
    }
  if(auto attack = std::get_if<PlayerAttack>(&*msg)) {
    onAttack(from, *attack, e.peer);
    return;
    }
  if(auto chat = std::get_if<Chat>(&*msg)) {
    const std::string line = sanitize(chat->text);
    sendOthers(e.peer, Chat{from, line});
    chatLine(from, line);
    }
  }

void NetSession::onHello(NetTransport::PeerId peer, const Hello& hello) {
  if(auto r = checkHello(hello)) {
    reject(peer, *r);
    return;
    }
  if(players.size()>=MaxPlayers) {
    reject(peer, Reject{RejectReason::ServerFull, Version, std::to_string(MaxPlayers) + " players"});
    return;
    }

  const PlayerId id = nextPlayer++;
  // the newcomer learns who is here, then is welcomed; the others learn about the newcomer
  for(auto& [pid,pname]:players)
    send(peer, PlayerJoined{pid, pname});
  send(peer, Welcome{id, world, nowMs()-startTime});
  for(auto a:avatars) {
    a.second.flags &= uint8_t(~Avatar::Respawn); // a newcomer spawns the characters, alive anyway
    send(peer, a.second);
    }
  if(worldTime)
    send(peer, WorldTime{*worldTime});
  for(auto& [eid,e]:entityMap)
    send(peer, e);
  sendOthers(peer, PlayerJoined{id, hello.name});

  peers[peer] = id;
  players[id] = hello.name;
  notify(hello.name + " joined the game");
  }

void NetSession::reject(NetTransport::PeerId peer, const Reject& r) {
  send(peer, r);
  net.disconnect(peer); // the queued Reject goes out first
  }

void NetSession::clientEvent(const NetTransport::Event& e) {
  if(e.peer!=hostPeer || st==State::Closed)
    return;
  switch(e.type) {
    case NetTransport::EventType::Connect:
      send(hostPeer, Hello{Version, name});
      return;
    case NetTransport::EventType::Disconnect:
      if(st==State::Online)
        notify("Connection to the host is lost"); else
        notify("Unable to connect to the host");
      st = State::Closed;
      clearWorld();
      return;
    case NetTransport::EventType::Receive:
      break;
    }

  auto msg = decode(e.data, e.size);
  if(!msg)
    return;

  if(auto m = std::get_if<Reject>(&*msg)) {
    notify(describe(*m));
    st = State::Closed;
    clearWorld();
    return;
    }
  if(auto m = std::get_if<Welcome>(&*msg)) {
    if(st!=State::Connecting)
      return;
    st             = State::Online;
    self           = m->playerId;
    players[self]  = name;
    notify("Joined the game as " + name + ", players online: " + std::to_string(players.size()));
    return;
    }
  if(auto m = std::get_if<PlayerJoined>(&*msg)) {
    players[m->playerId] = m->name;
    if(st==State::Online)
      notify(m->name + " joined the game");
    return;
    }
  if(auto m = std::get_if<PlayerLeft>(&*msg)) {
    if(st==State::Online && players.count(m->playerId))
      notify(std::string(playerName(m->playerId)) + " left the game");
    forgetPlayer(m->playerId);
    return;
    }
  if(auto m = std::get_if<PlayerSpawn>(&*msg)) {
    if(st!=State::Online || players.count(m->playerId)==0)
      return;
    avatars[m->playerId] = *m;
    if((m->flags & Avatar::Respawn) && respawns.size()<MaxPending)
      respawns.push_back(*m);
    return;
    }
  if(auto m = std::get_if<PlayerState>(&*msg)) {
    if(st==State::Online)
      onPlayerState(m->playerId, *m, hostPeer);
    return;
    }
  if(auto m = std::get_if<WorldTime>(&*msg)) {
    if(st==State::Online)
      worldTime = m->time;
    return;
    }
  if(auto m = std::get_if<PlayerAttack>(&*msg)) {
    if(st==State::Online)
      onAttack(m->playerId, *m, hostPeer);
    return;
    }
  if(auto m = std::get_if<Hit>(&*msg)) {
    if(st==State::Online && hits.size()<MaxPending)
      hits.push_back(*m);
    return;
    }
  if(auto m = std::get_if<SpawnEntity>(&*msg)) {
    if(st==State::Online) {
      entityMap[m->entityId] = *m;
      ++entityVersion;
      }
    return;
    }
  if(auto m = std::get_if<DespawnEntity>(&*msg)) {
    if(st==State::Online && entityMap.erase(m->entityId)>0)
      ++entityVersion;
    return;
    }
  if(auto m = std::get_if<NpcStates>(&*msg)) {
    if(st!=State::Online)
      return;
    for(auto& n:m->npcs)
      npcStates.push_back(n);
    if(npcStates.size()>MaxPendingNpcStates)
      npcStates.erase(npcStates.begin(), npcStates.end()-ptrdiff_t(MaxPendingNpcStates));
    return;
    }
  if(auto m = std::get_if<Chat>(&*msg)) {
    if(st==State::Online)
      chatLine(m->playerId, sanitize(m->text));
    return;
    }
  }

void NetSession::send(NetTransport::PeerId peer, const Message& msg, NetTransport::Channel ch) {
  auto pkg = encode(msg);
  net.send(peer, ch, pkg.data(), pkg.size());
  }

void NetSession::sendOthers(NetTransport::PeerId except, const Message& msg, NetTransport::Channel ch) {
  auto pkg = encode(msg);
  for(auto& [peer,id]:peers)
    if(peer!=except && id!=NoPlayer)
      net.send(peer, ch, pkg.data(), pkg.size());
  }

void NetSession::notify(const std::string& text) {
  if(onMessage)
    onMessage(text);
  }

void NetSession::chatLine(PlayerId from, std::string_view text) {
  auto who = playerName(from);
  std::string line = who.empty() ? "Player " + std::to_string(from) : std::string(who);
  line += ": ";
  line += text;
  notify(line);
  }
