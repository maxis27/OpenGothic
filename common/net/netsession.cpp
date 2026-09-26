#include "netsession.h"

#include <chrono>

using namespace NetProtocol;

namespace {

// a client gives up when it isn't welcomed by then (ENet alone would keep trying for ~30 s)
constexpr uint64_t ConnectTimeoutMs = 5000;

uint64_t nowMs() {
  using namespace std::chrono;
  return uint64_t(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
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

void NetSession::forgetPlayer(PlayerId id) {
  players.erase(id);
  avatars.erase(id);
  states .erase(id);
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
  for(auto& [pid,a]:avatars)
    send(peer, a);
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
      players.clear();
      avatars.clear();
      states.clear();
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
    players.clear();
    avatars.clear();
    states.clear();
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
    if(st==State::Online && players.count(m->playerId))
      avatars[m->playerId] = *m;
    return;
    }
  if(auto m = std::get_if<PlayerState>(&*msg)) {
    if(st==State::Online)
      onPlayerState(m->playerId, *m, hostPeer);
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
