// NetSession test, run in a single process: a host and two clients on localhost.
//  - both clients are welcomed and know every player's name;
//  - a chat line typed by one player shows up at every other player, with the sender's name;
//  - the characters the host announces (setAvatar) reach every client, newcomers included,
//    and only a new entity id is sent again;
//  - the character states a player sends reach every other player (through the host),
//    no more often than StateIntervalMs, and only newer ones replace older ones;
//  - the host's world time reaches every client, newcomers included, at most every
//    WorldTimeIntervalMs unless the clock has jumped;
//  - attacks reach every other player (through the host), only with the sender's own character;
//    the host's hits reach every client;
//  - a client leaving is announced to the others.
// Usage: NetSessionTest <port>. Exits with 0 on success.

#include "net/netsession.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

constexpr auto Timeout = std::chrono::seconds(10);

int failures = 0;

void check(bool cond, const char* what) {
  if(cond)
    return;
  std::fprintf(stderr, "FAILED: %s\n", what);
  ++failures;
  }

struct Player {
  std::unique_ptr<NetSession> session;
  std::vector<std::string>    lines;

  void attach(const char* tag) {
    session->onMessage = [this,tag](std::string_view s) {
      std::printf("[%s] %.*s\n", tag, int(s.size()), s.data());
      lines.emplace_back(s);
      };
    }

  bool saw(const std::string& line) const {
    for(auto& l:lines)
      if(l==line)
        return true;
    return false;
    }
  };

template<class Pred>
bool runUntil(std::vector<Player*> all, Pred done) {
  auto deadline = Clock::now() + Timeout;
  while(Clock::now()<deadline) {
    for(auto p:all)
      if(p->session)
        p->session->poll();
    if(done())
      return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
  return false;
  }

void testSession(uint16_t port) {
  Player host, diego, milten;
  host.session = NetSession::host(port, "Lee", "NEWWORLD.ZEN");
  if(host.session==nullptr) {
    check(false, "host listens");
    return;
    }
  host.attach("Lee");
  check(host.session->isHost() && host.session->state()==NetSession::State::Online, "host is online at once");

  diego.session = NetSession::connect("127.0.0.1", port, "Diego");
  check(diego.session!=nullptr, "first client starts connecting");
  if(diego.session==nullptr)
    return;
  diego.attach("Diego");

  bool ok = runUntil({&host,&diego}, [&]{ return diego.session->state()==NetSession::State::Online; });
  check(ok, "first client is welcomed");

  // the host spawned characters for itself and Diego before Milten came
  host.session->setAvatar({NetSession::HostPlayer, 10, 1, 2, 3, 90});
  host.session->setAvatar({diego.session->playerId(), 11, 4, 5, 6, 180});
  host.session->setAvatar({99, 12, 0, 0, 0, 0}); // no such player
  diego.session->setAvatar({diego.session->playerId(), 13, 0, 0, 0, 0}); // clients can't
  ok = runUntil({&host,&diego}, [&]{
    auto a = diego.session->avatar(diego.session->playerId());
    return diego.session->avatar(NetSession::HostPlayer)!=nullptr && a!=nullptr && a->entityId==11;
    });
  check(ok, "client learns the characters of the host and its own");
  check(diego.session->avatar(NetSession::HostPlayer)->entityId==10 &&
        diego.session->avatar(NetSession::HostPlayer)->rotation==90, "character data arrives intact");
  check(host.session->avatar(99)==nullptr, "no character for an unknown player");

  // world time: 8:00 on day 0, then a minute later (too soon to send), then after sleeping
  const int64_t hour = 60*60*1000;
  std::optional<int64_t> time;
  host.session->setWorldTime(8*hour);
  diego.session->setWorldTime(20*hour); // clients can't
  ok = runUntil({&host,&diego}, [&]{ time = diego.session->takeWorldTime(); return time.has_value(); });
  check(ok && *time==8*hour, "client gets the host's time");
  check(!host.session->takeWorldTime(), "the host takes no time from anyone");
  host.session->setWorldTime(8*hour+60*1000);
  for(int i=0; i<50; ++i) { // let anything sent arrive, well within WorldTimeIntervalMs
    host.session->poll();
    diego.session->poll();
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
  check(!diego.session->takeWorldTime(), "the time is sent at most every WorldTimeIntervalMs");
  host.session->setWorldTime(22*hour);
  ok = runUntil({&host,&diego}, [&]{ time = diego.session->takeWorldTime(); return time.has_value(); });
  check(ok && *time==22*hour, "a jump of the clock is sent at once");
  check(!diego.session->takeWorldTime(), "a time is taken only once");

  milten.session = NetSession::connect("127.0.0.1", port, "Milten");
  check(milten.session!=nullptr, "second client starts connecting");
  if(milten.session==nullptr)
    return;
  milten.attach("Milten");

  ok = runUntil({&host,&diego,&milten}, [&]{
    return milten.session->state()==NetSession::State::Online && diego.saw("Milten joined the game");
    });
  check(ok, "second client is welcomed and announced");
  check(host.session->playerCount()==3 && diego.session->playerCount()==3 && milten.session->playerCount()==3,
        "everyone knows all three players");
  check(milten.session->playerName(NetSession::HostPlayer)=="Lee", "clients know the host's name");
  check(milten.session->playerName(diego.session->playerId())=="Diego", "a newcomer knows who was there before");
  check(diego.session->playerId()!=milten.session->playerId() &&
        diego.session->playerId()!=NetSession::HostPlayer, "players get distinct ids");
  check(host.saw("Diego joined the game") && host.saw("Milten joined the game"), "host announces newcomers");
  check(!milten.saw("Diego joined the game"), "players already there are not announced as newcomers");

  ok = runUntil({&host,&diego,&milten}, [&]{
    return milten.session->avatar(NetSession::HostPlayer)!=nullptr && milten.session->avatar(diego.session->playerId())!=nullptr;
    });
  check(ok, "a newcomer gets the characters already in the world");
  check(milten.session->avatar(milten.session->playerId())==nullptr, "the newcomer's own character isn't spawned yet");
  time = milten.session->takeWorldTime();
  check(time && *time==22*hour, "a newcomer gets the host's time");

  host.session->setAvatar({milten.session->playerId(), 14, 7, 8, 9, 0});
  // the same id again (a moved character) is not resent, a new id is
  host.session->setAvatar({NetSession::HostPlayer, 10, 50, 50, 50, 0});
  host.session->setAvatar({diego.session->playerId(), 15, 4, 5, 6, 180});
  ok = runUntil({&host,&diego,&milten}, [&]{
    auto m = diego.session->avatar(milten.session->playerId());
    auto d = milten.session->avatar(diego.session->playerId());
    return m!=nullptr && m->entityId==14 && d!=nullptr && d->entityId==15;
    });
  check(ok, "new and respawned characters reach every client");
  check(diego.session->avatar(NetSession::HostPlayer)->x==1, "an unchanged id is not sent again");

  // client -> everyone
  check(diego.session->sendChat("Hello from the Old Camp"), "client sends chat");
  ok = runUntil({&host,&diego,&milten}, [&]{
    return host.saw("Diego: Hello from the Old Camp") && milten.saw("Diego: Hello from the Old Camp");
    });
  check(ok, "client chat reaches the host and the other client");
  check(diego.saw("Diego: Hello from the Old Camp"), "sender sees own chat line");

  // host -> everyone
  check(host.session->sendChat("Welcome to the New Camp"), "host sends chat");
  ok = runUntil({&host,&diego,&milten}, [&]{
    return diego.saw("Lee: Welcome to the New Camp") && milten.saw("Lee: Welcome to the New Camp");
    });
  check(ok, "host chat reaches both clients");

  // no echo back to the sender, no empty lines
  size_t diegoCount = 0;
  for(auto& l:diego.lines)
    if(l=="Diego: Hello from the Old Camp")
      ++diegoCount;
  check(diegoCount==1, "sender does not get its chat line back");
  check(!host.session->sendChat("   "), "blank chat is not sent");

  // character states: client -> host -> other client, and host -> clients
  const auto diegoId = diego.session->playerId();
  NetSession::PlayerState ds;
  ds.playerId = 77; // ignored, the sender's id goes out
  ds.entityId = 15;
  ds.x = 100; ds.y = 200; ds.z = 300; ds.rotation = 45;
  ds.bodyState = 3; ds.anim = 2; ds.walkMode = 1; ds.weaponState = 4;
  ds.meleeWeapon = 1200; ds.rangedWeapon = 1300;
  check(diego.session->sendPlayerState(ds), "client sends its state");
  check(!diego.session->sendPlayerState(ds), "the next state waits for StateIntervalMs");
  ds.entityId = 0;
  check(!host.session->sendPlayerState(ds), "a state without a character is not sent");
  ok = runUntil({&host,&diego,&milten}, [&]{
    return host.session->playerState(diegoId)!=nullptr && milten.session->playerState(diegoId)!=nullptr;
    });
  check(ok, "client state reaches the host and the other client");
  if(ok) {
    auto m = milten.session->playerState(diegoId);
    check(m->playerId==diegoId && m->entityId==15 && m->x==100 && m->y==200 && m->z==300 && m->rotation==45 &&
          m->bodyState==3 && m->anim==2 && m->walkMode==1 && m->weaponState==4 &&
          m->meleeWeapon==1200 && m->rangedWeapon==1300 && m->seq==1,
          "state arrives intact with the sender's id");
    }
  check(diego.session->playerState(diegoId)==nullptr, "a player gets no state of its own back");

  NetSession::PlayerState hs;
  hs.entityId = 10;
  hs.x = -5;
  check(host.session->sendPlayerState(hs), "host sends its state");
  ok = runUntil({&host,&diego,&milten}, [&]{
    auto d = diego.session->playerState(NetSession::HostPlayer);
    auto m = milten.session->playerState(NetSession::HostPlayer);
    return d!=nullptr && d->x==-5 && m!=nullptr && m->x==-5;
    });
  check(ok, "host state reaches both clients");

  std::this_thread::sleep_for(std::chrono::milliseconds(NetSession::StateIntervalMs+10));
  ds.entityId = 15;
  ds.x = 110;
  check(diego.session->sendPlayerState(ds), "client sends again after StateIntervalMs");
  ok = runUntil({&host,&diego,&milten}, [&]{
    auto m = milten.session->playerState(diegoId);
    return m!=nullptr && m->x==110 && m->seq==2;
    });
  check(ok, "a newer state replaces the older one");

  // attacks: client -> host -> other client, and host -> clients
  std::vector<NetSession::PlayerAttack> hostAtk, diegoAtk, miltenAtk;
  auto collectAttacks = [&]{
    for(auto& a:host.session->takeAttacks())   hostAtk.push_back(a);
    for(auto& a:diego.session->takeAttacks())  diegoAtk.push_back(a);
    for(auto& a:milten.session->takeAttacks()) miltenAtk.push_back(a);
    };
  check(diego.session->sendAttack({77, 15, 0, 10, NetProtocol::AttackMove::SwingLeft}), "client sends an attack");
  check(diego.session->sendAttack({77, 11, 0, 10, NetProtocol::AttackMove::Swing}), "client sends an attack with a foreign character");
  check(!diego.session->sendAttack({77, 0, 0, 10, NetProtocol::AttackMove::Swing}), "an attack without a character is not sent");
  check(host.session->sendAttack({0, 10, 0, 15, NetProtocol::AttackMove::Parade}), "host sends an attack");
  ok = runUntil({&host,&diego,&milten}, [&]{
    collectAttacks();
    return !hostAtk.empty() && !diegoAtk.empty() && miltenAtk.size()>=2;
    });
  check(ok, "attacks reach the host and the other players");
  for(int i=0; i<50; ++i) { // let anything else sent arrive
    for(auto p:{&host,&diego,&milten})
      p->session->poll();
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
  collectAttacks();
  check(hostAtk.size()==1 && hostAtk[0].playerId==diegoId && hostAtk[0].entityId==15 && hostAtk[0].target==10 &&
        hostAtk[0].move==NetProtocol::AttackMove::SwingLeft, "the host gets the client's attack intact");
  check(miltenAtk.size()==2, "an attack with another player's character is dropped");
  check(diegoAtk.size()==1 && diegoAtk[0].playerId==NetSession::HostPlayer && diegoAtk[0].move==NetProtocol::AttackMove::Parade,
        "a player gets no attack of its own back");

  // hits: host -> clients only
  host.session->sendHit({10, 15, 80, 20, NetSession::Hit::Effect|NetSession::Hit::Stumble});
  diego.session->sendHit({15, 10, 0, 999, 0}); // clients can't
  std::vector<NetSession::Hit> diegoHits, miltenHits;
  ok = runUntil({&host,&diego,&milten}, [&]{
    for(auto& h:diego.session->takeHits())  diegoHits.push_back(h);
    for(auto& h:milten.session->takeHits()) miltenHits.push_back(h);
    return !diegoHits.empty() && !miltenHits.empty();
    });
  check(ok, "hits reach every client");
  check(diegoHits.size()==1 && diegoHits[0].attacker==10 && diegoHits[0].target==15 && diegoHits[0].hp==80 &&
        diegoHits[0].damage==20 && diegoHits[0].flags==(NetSession::Hit::Effect|NetSession::Hit::Stumble),
        "a hit arrives intact");
  check(host.session->takeHits().empty(), "the host takes no hits from anyone");

  // leaving
  const auto miltenId = milten.session->playerId();
  milten.session.reset();
  ok = runUntil({&host,&diego}, [&]{ return diego.saw("Milten left the game") && host.saw("Milten left the game"); });
  check(ok, "a leaving client is announced");
  check(diego.session->avatar(miltenId)==nullptr && host.session->avatar(miltenId)==nullptr,
        "the character of a leaving player is forgotten");
  check(host.session->playerState(diegoId)!=nullptr, "states of the remaining players stay");
  check(host.session->playerCount()==2 && diego.session->playerCount()==2, "player lists shrink");
  check(diego.session->avatar(NetSession::HostPlayer)!=nullptr && host.session->avatar(NetSession::HostPlayer)!=nullptr,
        "characters of the remaining players stay");

  // host gone: the client notices and closes
  host.session.reset();
  ok = runUntil({&diego}, [&]{ return diego.session->state()==NetSession::State::Closed; });
  check(ok, "client notices the host is gone");
  check(!diego.session->sendChat("anyone?"), "no chat once closed");
  }

void testNoHost(uint16_t port) {
  // nobody listens on this port: the client gives up and reports it
  Player lost;
  lost.session = NetSession::connect("127.0.0.1", port, "Lester");
  if(lost.session==nullptr) {
    check(false, "client starts connecting");
    return;
    }
  lost.attach("Lester");
  bool ok = runUntil({&lost}, [&]{ return lost.session->state()==NetSession::State::Closed; });
  check(ok, "connecting to nobody fails");
  check(lost.saw("Unable to connect to the host"), "failure is reported");
  }

bool parsePort(const char* str, uint16_t& port) {
  char* end = nullptr;
  long  v   = std::strtol(str, &end, 10);
  if(end==str || *end!='\0' || v<=0 || v>65535)
    return false;
  port = uint16_t(v);
  return true;
  }

}

int main(int argc, char** argv) {
  uint16_t port = 0;
  if(argc!=2 || !parsePort(argv[1], port)) {
    std::fprintf(stderr, "usage: %s <port>\n", argv[0]);
    return 2;
    }
  testSession(port);
  testNoHost(port);
  if(failures>0) {
    std::fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
    }
  std::printf("all checks passed\n");
  return 0;
  }
