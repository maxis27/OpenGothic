#include "netprotocol.h"

#include <bit>
#include <cmath>
#include <limits>

using namespace NetProtocol;

namespace {

class Writer {
  public:
    void u8 (uint8_t v)  { buf.push_back(v); }
    void u16(uint16_t v) { put(v, 2); }
    void u32(uint32_t v) { put(v, 4); }
    void u64(uint64_t v) { put(v, 8); }
    void f32(float v)    { u32(std::bit_cast<uint32_t>(v)); }
    void str(const std::string& s, size_t maxLength) {
      const size_t len = s.size()<maxLength ? s.size() : maxLength;
      u16(uint16_t(len));
      buf.insert(buf.end(), s.begin(), s.begin()+ptrdiff_t(len));
      }

    std::vector<uint8_t> buf;

  private:
    void put(uint64_t v, size_t bytes) {
      for(size_t i=0; i<bytes; ++i)
        buf.push_back(uint8_t(v >> (8*i)));
      }
  };

class Reader {
  public:
    Reader(const uint8_t* data, size_t size):data(data), size(size) {}

    bool u8 (uint8_t&  v) { uint64_t x=0; if(!get(x,1)) return false; v = uint8_t (x); return true; }
    bool u16(uint16_t& v) { uint64_t x=0; if(!get(x,2)) return false; v = uint16_t(x); return true; }
    bool u32(uint32_t& v) { uint64_t x=0; if(!get(x,4)) return false; v = uint32_t(x); return true; }
    bool u64(uint64_t& v) { return get(v,8); }
    // only finite values: a NaN or infinity would poison positions in the world
    bool f32(float& v) { uint32_t x=0; if(!u32(x)) return false; v = std::bit_cast<float>(x); return std::isfinite(v); }
    bool str(std::string& s, size_t maxLength) {
      uint16_t len = 0;
      if(!u16(len) || len>maxLength || len>size-pos)
        return false;
      s.assign(reinterpret_cast<const char*>(data+pos), len);
      pos += len;
      return true;
      }

    bool atEnd() const { return pos==size; }

  private:
    bool get(uint64_t& v, size_t bytes) {
      if(bytes>size-pos)
        return false;
      v = 0;
      for(size_t i=0; i<bytes; ++i)
        v |= uint64_t(data[pos+i]) << (8*i);
      pos += bytes;
      return true;
      }

    const uint8_t* data = nullptr;
    size_t         size = 0;
    size_t         pos  = 0;
  };

void write(Writer& w, const Hello& m) {
  w.u8 (uint8_t(MsgType::Hello));
  w.u32(Magic);
  w.u16(m.version);
  w.str(m.name, MaxNameLength);
  }

void write(Writer& w, const Welcome& m) {
  w.u8 (uint8_t(MsgType::Welcome));
  w.u32(m.playerId);
  w.str(m.worldName, MaxWorldLength);
  w.u64(m.serverTick);
  }

void write(Writer& w, const Reject& m) {
  w.u8 (uint8_t(MsgType::Reject));
  w.u8 (uint8_t(m.reason));
  w.u16(m.serverVersion);
  w.str(m.text, MaxReasonLength);
  }

void write(Writer& w, const Chat& m) {
  w.u8 (uint8_t(MsgType::Chat));
  w.u32(m.playerId);
  w.str(m.text, MaxChatLength);
  }

void write(Writer& w, const PlayerJoined& m) {
  w.u8 (uint8_t(MsgType::PlayerJoined));
  w.u32(m.playerId);
  w.str(m.name, MaxNameLength);
  }

void write(Writer& w, const PlayerLeft& m) {
  w.u8 (uint8_t(MsgType::PlayerLeft));
  w.u32(m.playerId);
  }

void write(Writer& w, const PlayerSpawn& m) {
  w.u8 (uint8_t(MsgType::PlayerSpawn));
  w.u32(m.playerId);
  w.u32(m.entityId);
  w.f32(m.x);
  w.f32(m.y);
  w.f32(m.z);
  w.f32(m.rotation);
  w.u8 (m.flags);
  }

void write(Writer& w, const PlayerState& m) {
  w.u8 (uint8_t(MsgType::PlayerState));
  w.u32(m.playerId);
  w.u32(m.entityId);
  w.u32(m.seq);
  w.u32(m.time);
  w.f32(m.x);
  w.f32(m.y);
  w.f32(m.z);
  w.f32(m.rotation);
  w.u32(m.bodyState);
  w.u16(m.anim);
  w.u8 (m.walkMode);
  w.u8 (m.weaponState);
  w.u32(m.meleeWeapon);
  w.u32(m.rangedWeapon);
  }

void write(Writer& w, const WorldTime& m) {
  w.u8 (uint8_t(MsgType::WorldTime));
  w.u64(uint64_t(m.time));
  }

void write(Writer& w, const PlayerAttack& m) {
  w.u8 (uint8_t(MsgType::PlayerAttack));
  w.u32(m.playerId);
  w.u32(m.entityId);
  w.u32(m.time);
  w.u32(m.target);
  w.u8 (uint8_t(m.move));
  }

void write(Writer& w, const Hit& m) {
  w.u8 (uint8_t(MsgType::Hit));
  w.u32(m.attacker);
  w.u32(m.target);
  w.u32(uint32_t(m.hp));
  w.u32(uint32_t(m.damage));
  w.u8 (m.flags);
  }

std::optional<Message> readHello(Reader& r) {
  Hello    m;
  uint32_t magic = 0;
  if(!r.u32(magic) || magic!=Magic || !r.u16(m.version))
    return std::nullopt;
  if(m.version!=Version)
    return m; // layout past the version is unknown; checkHello() rejects it
  if(!r.str(m.name, MaxNameLength) || !r.atEnd())
    return std::nullopt;
  return m;
  }

std::optional<Message> readWelcome(Reader& r) {
  Welcome m;
  if(!r.u32(m.playerId) || !r.str(m.worldName, MaxWorldLength) || !r.u64(m.serverTick) || !r.atEnd())
    return std::nullopt;
  return m;
  }

std::optional<Message> readReject(Reader& r) {
  Reject  m;
  uint8_t reason = 0;
  // no atEnd() check: a newer server may append fields, the frozen prefix is still readable
  if(!r.u8(reason) || !r.u16(m.serverVersion) || !r.str(m.text, MaxReasonLength))
    return std::nullopt;
  m.reason = RejectReason(reason);
  return m;
  }

std::optional<Message> readChat(Reader& r) {
  Chat m;
  if(!r.u32(m.playerId) || !r.str(m.text, MaxChatLength) || !r.atEnd())
    return std::nullopt;
  return m;
  }

std::optional<Message> readPlayerJoined(Reader& r) {
  PlayerJoined m;
  if(!r.u32(m.playerId) || !r.str(m.name, MaxNameLength) || !r.atEnd())
    return std::nullopt;
  return m;
  }

std::optional<Message> readPlayerLeft(Reader& r) {
  PlayerLeft m;
  if(!r.u32(m.playerId) || !r.atEnd())
    return std::nullopt;
  return m;
  }

std::optional<Message> readPlayerSpawn(Reader& r) {
  PlayerSpawn m;
  if(!r.u32(m.playerId) || !r.u32(m.entityId) || m.entityId==0 ||
     !r.f32(m.x) || !r.f32(m.y) || !r.f32(m.z) || !r.f32(m.rotation) ||
     !r.u8(m.flags) || (m.flags & ~PlayerSpawn::AllFlags)!=0 || !r.atEnd())
    return std::nullopt;
  return m;
  }

std::optional<Message> readPlayerState(Reader& r) {
  PlayerState m;
  if(!r.u32(m.playerId) || !r.u32(m.entityId) || m.entityId==0 || !r.u32(m.seq) || !r.u32(m.time) ||
     !r.f32(m.x) || !r.f32(m.y) || !r.f32(m.z) || !r.f32(m.rotation) ||
     !r.u32(m.bodyState) || !r.u16(m.anim) || !r.u8(m.walkMode) || !r.u8(m.weaponState) ||
     !r.u32(m.meleeWeapon) || !r.u32(m.rangedWeapon) || !r.atEnd())
    return std::nullopt;
  return m;
  }

std::optional<Message> readWorldTime(Reader& r) {
  uint64_t time = 0;
  if(!r.u64(time) || time>uint64_t(std::numeric_limits<int64_t>::max()) || !r.atEnd())
    return std::nullopt;
  return WorldTime{int64_t(time)};
  }

std::optional<Message> readPlayerAttack(Reader& r) {
  PlayerAttack m;
  uint8_t      move = 0;
  if(!r.u32(m.playerId) || !r.u32(m.entityId) || m.entityId==0 || !r.u32(m.time) || !r.u32(m.target) ||
     !r.u8(move) || move<uint8_t(AttackMove::Swing) || move>uint8_t(AttackMove::Finish) || !r.atEnd())
    return std::nullopt;
  m.move = AttackMove(move);
  return m;
  }

std::optional<Message> readHit(Reader& r) {
  Hit      m;
  uint32_t hp = 0, damage = 0;
  if(!r.u32(m.attacker) || !r.u32(m.target) || m.target==0 || !r.u32(hp) || !r.u32(damage) ||
     !r.u8(m.flags) || (m.flags & ~Hit::AllFlags)!=0 || !r.atEnd())
    return std::nullopt;
  if((m.flags & Hit::Dead) && (m.flags & Hit::Unconscious))
    return std::nullopt;
  m.hp     = int32_t(hp);
  m.damage = int32_t(damage);
  if(m.hp<0 || m.damage<0)
    return std::nullopt;
  return m;
  }

bool isValidName(const std::string& name) {
  if(name.empty() || name.size()>MaxNameLength)
    return false;
  for(char c:name)
    if(uint8_t(c)<0x20 || c==0x7F)
      return false;
  return true;
  }

}

std::vector<uint8_t> NetProtocol::encode(const Message& msg) {
  Writer w;
  std::visit([&](const auto& m){ write(w, m); }, msg);
  return std::move(w.buf);
  }

std::optional<Message> NetProtocol::decode(const uint8_t* data, size_t size) {
  Reader  r(data, size);
  uint8_t type = 0;
  if(!r.u8(type))
    return std::nullopt;
  switch(MsgType(type)) {
    case MsgType::Hello:   return readHello(r);
    case MsgType::Welcome: return readWelcome(r);
    case MsgType::Reject:  return readReject(r);
    case MsgType::Chat:    return readChat(r);
    case MsgType::PlayerJoined: return readPlayerJoined(r);
    case MsgType::PlayerLeft:   return readPlayerLeft(r);
    case MsgType::PlayerSpawn:  return readPlayerSpawn(r);
    case MsgType::PlayerState:  return readPlayerState(r);
    case MsgType::WorldTime:    return readWorldTime(r);
    case MsgType::PlayerAttack: return readPlayerAttack(r);
    case MsgType::Hit:          return readHit(r);
    }
  return std::nullopt;
  }

std::optional<Reject> NetProtocol::checkHello(const Hello& hello) {
  if(hello.version!=Version) {
    Reject r;
    r.reason = RejectReason::VersionMismatch;
    r.text   = "server uses protocol version " + std::to_string(Version) +
               ", client uses " + std::to_string(hello.version);
    return r;
    }
  if(!isValidName(hello.name)) {
    Reject r;
    r.reason = RejectReason::BadName;
    r.text   = "player name must be 1-" + std::to_string(MaxNameLength) + " printable characters";
    return r;
    }
  return std::nullopt;
  }

std::string NetProtocol::describe(const Reject& reject, uint16_t clientVersion) {
  // built from our own version and the server's, so a mismatch is explained even to an older server
  switch(reject.reason) {
    case RejectReason::VersionMismatch:
      return "Incompatible multiplayer version: server uses protocol " + std::to_string(reject.serverVersion) +
             ", this game uses " + std::to_string(clientVersion);
    case RejectReason::BadName:
      return "Connection refused: invalid player name" + (reject.text.empty() ? "" : " (" + reject.text + ")");
    case RejectReason::ServerFull:
      return "Connection refused: server is full";
    case RejectReason::Malformed:
      break;
    }
  return "Connection refused by server" + (reject.text.empty() ? "" : ": " + reject.text);
  }
