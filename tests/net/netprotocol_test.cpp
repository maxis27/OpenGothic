// NetProtocol test, run in a single process:
//  - every message survives an encode/decode round trip, malformed packets are refused;
//  - over a real NetTransport on localhost, a client with the current protocol version is
//    welcomed and can chat, while a client with another version is rejected with a message
//    and disconnected.
// Usage: NetProtocolTest <port>. Exits with 0 on success.

#include "net/netprotocol.h"
#include "net/nettransport.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

using namespace NetProtocol;

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

template<class T>
const T* roundTrip(const T& msg, std::optional<Message>& storage) {
  auto pkg = encode(msg);
  storage = decode(pkg.data(), pkg.size());
  return storage ? std::get_if<T>(&*storage) : nullptr;
  }

void testEncoding() {
  std::optional<Message> s;

  auto hello = roundTrip(Hello{Version, "Diego"}, s);
  check(hello!=nullptr && hello->version==Version && hello->name=="Diego", "Hello round trip");

  auto welcome = roundTrip(Welcome{7, "NEWWORLD.ZEN", 0x1122334455667788ull}, s);
  check(welcome!=nullptr && welcome->playerId==7 && welcome->worldName=="NEWWORLD.ZEN" &&
        welcome->serverTick==0x1122334455667788ull, "Welcome round trip");

  auto reject = roundTrip(Reject{RejectReason::ServerFull, Version, "8/8"}, s);
  check(reject!=nullptr && reject->reason==RejectReason::ServerFull && reject->text=="8/8", "Reject round trip");

  auto chat = roundTrip(Chat{3, "Hello, Khorinis! \xC5\xBC\xC3\xB3\xC5\x82w"}, s);
  check(chat!=nullptr && chat->playerId==3 && chat->text=="Hello, Khorinis! \xC5\xBC\xC3\xB3\xC5\x82w", "Chat round trip");

  auto joined = roundTrip(PlayerJoined{4, "Lares"}, s);
  check(joined!=nullptr && joined->playerId==4 && joined->name=="Lares", "PlayerJoined round trip");

  auto left = roundTrip(PlayerLeft{4}, s);
  check(left!=nullptr && left->playerId==4, "PlayerLeft round trip");

  auto spawn = roundTrip(PlayerSpawn{4, 17, 1.5f, -200.25f, 3e4f, 270.f, PlayerSpawn::Respawn}, s);
  check(spawn!=nullptr && spawn->playerId==4 && spawn->entityId==17 && spawn->x==1.5f &&
        spawn->y==-200.25f && spawn->z==3e4f && spawn->rotation==270.f && spawn->flags==PlayerSpawn::Respawn,
        "PlayerSpawn round trip");
  auto badSpawn = encode(PlayerSpawn{4, 17, 0, 0, 0, 0, 0x02});
  check(!decode(badSpawn.data(), badSpawn.size()), "PlayerSpawn with unknown flags is refused");
  auto nanSpawn = encode(PlayerSpawn{4, 17, std::numeric_limits<float>::quiet_NaN(), 0, 0, 0});
  check(!decode(nanSpawn.data(), nanSpawn.size()), "PlayerSpawn with NaN position is refused");
  auto noIdSpawn = encode(PlayerSpawn{4, 0, 0, 0, 0, 0});
  check(!decode(noIdSpawn.data(), noIdSpawn.size()), "PlayerSpawn without entity id is refused");

  auto state = roundTrip(PlayerState{4, 17, 0xFFFFFFF0u, 123456, -1.f, 2.5f, 3e5f, 90.f, 0x18003, 2, 1, 3, 0xABCD, 0xFFFFFFFFu}, s);
  check(state!=nullptr && state->playerId==4 && state->entityId==17 && state->seq==0xFFFFFFF0u &&
        state->time==123456 && state->x==-1.f && state->y==2.5f && state->z==3e5f && state->rotation==90.f &&
        state->bodyState==0x18003 && state->anim==2 && state->walkMode==1 && state->weaponState==3 &&
        state->meleeWeapon==0xABCD && state->rangedWeapon==0xFFFFFFFFu,
        "PlayerState round trip");
  auto infState = encode(PlayerState{4, 17, 1, 0, 0, 0, 0, std::numeric_limits<float>::infinity()});
  check(!decode(infState.data(), infState.size()), "PlayerState with infinite rotation is refused");
  auto noIdState = encode(PlayerState{4, 0, 1});
  check(!decode(noIdState.data(), noIdState.size()), "PlayerState without entity id is refused");
  auto cutState = encode(PlayerState{4, 17, 1});
  check(!decode(cutState.data(), cutState.size()-1), "truncated PlayerState is refused");

  // day 12, 21:37 in game milliseconds
  auto time = roundTrip(WorldTime{((12*24+21)*60+37)*60000ll}, s);
  check(time!=nullptr && time->time==((12*24+21)*60+37)*60000ll, "WorldTime round trip");
  auto negTime = encode(WorldTime{-1});
  check(!decode(negTime.data(), negTime.size()), "negative WorldTime is refused");
  auto cutTime = encode(WorldTime{1});
  check(!decode(cutTime.data(), cutTime.size()-1), "truncated WorldTime is refused");

  auto attack = roundTrip(PlayerAttack{3, 17, 98765, 21, AttackMove::SwingLeft}, s);
  check(attack!=nullptr && attack->playerId==3 && attack->entityId==17 && attack->time==98765 &&
        attack->target==21 && attack->move==AttackMove::SwingLeft, "PlayerAttack round trip");
  auto noIdAttack = encode(PlayerAttack{3, 0, 1, 0, AttackMove::Swing});
  check(!decode(noIdAttack.data(), noIdAttack.size()), "PlayerAttack without entity id is refused");
  auto shot = roundTrip(PlayerAttack{3, 17, 98765, 0, AttackMove::Shoot, 2.5f, 0.25f, -1.5f}, s);
  check(shot!=nullptr && shot->move==AttackMove::Shoot && shot->target==0 &&
        shot->dx==2.5f && shot->dy==0.25f && shot->dz==-1.5f, "PlayerAttack shot round trip");
  auto nanShot = encode(PlayerAttack{3, 17, 1, 0, AttackMove::Shoot, std::nanf(""), 0, 0});
  check(!decode(nanShot.data(), nanShot.size()), "PlayerAttack shot with NaN direction is refused");
  auto badMove = encode(PlayerAttack{3, 17, 1, 0, AttackMove(7)});
  check(!decode(badMove.data(), badMove.size()), "PlayerAttack with unknown move is refused");
  auto cutAttack = encode(PlayerAttack{3, 17, 1, 0, AttackMove::Finish});
  check(!decode(cutAttack.data(), cutAttack.size()-1), "truncated PlayerAttack is refused");

  auto hit = roundTrip(Hit{17, 21, 115, 45, Hit::Effect|Hit::Stumble|Hit::DontKill}, s);
  check(hit!=nullptr && hit->attacker==17 && hit->target==21 && hit->hp==115 && hit->damage==45 &&
        hit->flags==(Hit::Effect|Hit::Stumble|Hit::DontKill), "Hit round trip");
  auto noTargetHit = encode(Hit{17, 0, 1, 1, 0});
  check(!decode(noTargetHit.data(), noTargetHit.size()), "Hit without target is refused");
  auto negHit = encode(Hit{17, 21, -5, 1, 0});
  check(!decode(negHit.data(), negHit.size()), "Hit with negative hit points is refused");
  auto dead = roundTrip(Hit{17, 21, 0, 3, Hit::Effect|Hit::Dead}, s);
  check(dead!=nullptr && dead->hp==0 && dead->flags==(Hit::Effect|Hit::Dead), "Hit felling the target round trip");
  auto badFlags = encode(Hit{17, 21, 0, 1, Hit::Dead|Hit::Unconscious});
  check(!decode(badFlags.data(), badFlags.size()), "Hit leaving the target both dead and unconscious is refused");
  auto cutHit = encode(Hit{17, 21, 1, 1, 0});
  check(!decode(cutHit.data(), cutHit.size()-1), "truncated Hit is refused");

  // too long strings are cut on encode
  auto longChat = roundTrip(Chat{1, std::string(MaxChatLength+100, 'a')}, s);
  check(longChat!=nullptr && longChat->text.size()==MaxChatLength, "Chat text is limited");

  // a Hello from another version decodes to just its version, whatever follows it
  auto future = encode(Hello{uint16_t(Version+1), "Lester"});
  future.push_back(0xFF);
  s = decode(future.data(), future.size());
  hello = s ? std::get_if<Hello>(&*s) : nullptr;
  check(hello!=nullptr && hello->version==Version+1, "Hello with foreign version decodes");

  // truncated packets are refused
  auto pkg = encode(Welcome{1, "WORLD", 42});
  for(size_t i=0; i<pkg.size(); ++i)
    check(!decode(pkg.data(), i), "truncated Welcome is refused");

  // trailing garbage, unknown type, wrong magic
  pkg.push_back(0);
  check(!decode(pkg.data(), pkg.size()), "Welcome with trailing bytes is refused");
  const uint8_t unknown[] = {0xEE, 1, 2, 3};
  check(!decode(unknown, sizeof(unknown)), "unknown message type is refused");
  auto badMagic = encode(Hello{Version, "Milten"});
  badMagic[1] ^= 0xFF;
  check(!decode(badMagic.data(), badMagic.size()), "Hello with wrong magic is refused");

  // string length beyond the limit
  auto bigName = encode(Hello{Version, "x"});
  bigName[7] = uint8_t(MaxNameLength+1);
  check(!decode(bigName.data(), bigName.size()), "oversized name is refused");

  // server-side handshake checks
  check(!checkHello(Hello{Version, "Gorn"}), "valid Hello is accepted");
  auto r = checkHello(Hello{uint16_t(Version+1), "Gorn"});
  check(r && r->reason==RejectReason::VersionMismatch && r->serverVersion==Version, "foreign version is rejected");
  r = checkHello(Hello{Version, ""});
  check(r && r->reason==RejectReason::BadName, "empty name is rejected");
  r = checkHello(Hello{Version, "Go\nrn"});
  check(r && r->reason==RejectReason::BadName, "name with control characters is rejected");
  }

// Minimal listen server: answers Hello with Welcome or Reject and relays chat to everyone.
class Server {
  public:
    bool start(uint16_t port) { return net.host(port, 4); }

    void poll() {
      net.poll([&](const NetTransport::Event& e) {
        if(e.type==NetTransport::EventType::Disconnect)
          players.erase(e.peer);
        if(e.type!=NetTransport::EventType::Receive)
          return;
        auto msg = decode(e.data, e.size);
        if(!msg) {
          reject(e.peer, Reject{RejectReason::Malformed, Version, "malformed packet"});
          return;
          }
        if(auto hello = std::get_if<Hello>(&*msg)) {
          if(auto r = checkHello(*hello)) {
            reject(e.peer, *r);
            return;
            }
          const uint32_t id = nextPlayer++;
          players[e.peer] = id;
          send(e.peer, Welcome{id, "NEWWORLD.ZEN", 1000});
          return;
          }
        if(auto chat = std::get_if<Chat>(&*msg)) {
          auto it = players.find(e.peer);
          if(it==players.end())
            return; // no chat before the handshake
          auto pkg = encode(Chat{it->second, chat->text});
          for(auto& [peer,id]:players)
            net.send(peer, NetTransport::Reliable, pkg.data(), pkg.size());
          }
        });
      }

    size_t rejected = 0;

  private:
    void send(NetTransport::PeerId peer, const Message& msg) {
      auto pkg = encode(msg);
      net.send(peer, NetTransport::Reliable, pkg.data(), pkg.size());
      }

    void reject(NetTransport::PeerId peer, const Reject& r) {
      send(peer, r);
      net.disconnect(peer); // delivers the queued Reject first
      ++rejected;
      }

    NetTransport net;
    uint32_t     nextPlayer = 1;
    std::unordered_map<NetTransport::PeerId,uint32_t> players;
  };

// Client that sends Hello with the given version and records what the server answered.
struct Client {
  NetTransport         net;
  NetTransport::PeerId server = NetTransport::InvalidPeer;
  uint16_t             version = Version;
  std::string          name;

  bool                   connected    = false;
  bool                   disconnected = false;
  std::optional<Welcome> welcome;
  std::optional<Reject>  reject;
  std::vector<Chat>      chat;

  bool start(uint16_t port) {
    server = net.connect("127.0.0.1", port);
    return server!=NetTransport::InvalidPeer;
    }

  void send(const Message& msg) {
    auto pkg = encode(msg);
    net.send(server, NetTransport::Reliable, pkg.data(), pkg.size());
    }

  void poll() {
    net.poll([&](const NetTransport::Event& e) {
      switch(e.type) {
        case NetTransport::EventType::Connect:
          connected = true;
          send(Hello{version, name});
          break;
        case NetTransport::EventType::Disconnect:
          disconnected = true;
          break;
        case NetTransport::EventType::Receive: {
          auto msg = decode(e.data, e.size);
          if(!msg)
            break;
          if(auto m = std::get_if<Welcome>(&*msg))
            welcome = *m;
          if(auto m = std::get_if<Reject>(&*msg))
            reject = *m;
          if(auto m = std::get_if<Chat>(&*msg))
            chat.push_back(*m);
          break;
          }
        }
      });
    }
  };

template<class Pred>
bool runUntil(Server& srv, Client& a, Client& b, Pred done) {
  auto deadline = Clock::now() + Timeout;
  while(Clock::now()<deadline) {
    srv.poll();
    a.poll();
    b.poll();
    if(done())
      return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
  return false;
  }

void testHandshake(uint16_t port) {
  Server srv;
  if(!srv.start(port)) {
    check(false, "server listens");
    return;
    }

  Client good, old;
  good.name    = "Vatras";
  old.name     = "Xardas";
  old.version  = uint16_t(Version+1);
  check(good.start(port), "good client connects");
  check(old.start(port),  "outdated client connects");

  bool ok = runUntil(srv, good, old, [&]{ return good.welcome && old.reject && old.disconnected; });
  check(ok, "handshake finishes in time");
  check(good.welcome && good.welcome->worldName=="NEWWORLD.ZEN", "good client is welcomed");
  check(!good.reject, "good client is not rejected");
  check(!old.welcome, "outdated client is not welcomed");
  check(old.reject && old.reject->reason==RejectReason::VersionMismatch, "outdated client gets VersionMismatch");
  check(srv.rejected==1, "server rejected exactly one client");
  if(old.reject) {
    const std::string text = describe(*old.reject, old.version);
    std::printf("outdated client: %s\n", text.c_str());
    check(text.find(std::to_string(Version))!=std::string::npos &&
          text.find(std::to_string(Version+1))!=std::string::npos, "rejection message names both versions");
    }

  good.send(Chat{0, "Hi there"});
  ok = runUntil(srv, good, old, [&]{ return !good.chat.empty(); });
  check(ok, "chat is relayed in time");
  check(!good.chat.empty() && good.chat[0].text=="Hi there" && good.welcome &&
        good.chat[0].playerId==good.welcome->playerId, "chat carries the sender's player id");
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
  testEncoding();
  testHandshake(port);
  if(failures>0) {
    std::fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
    }
  std::printf("all checks passed\n");
  return 0;
  }
