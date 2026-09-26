#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

// Binary message format of the multiplayer protocol, independent of the transport.
//
// Every packet starts with a one-byte MsgType followed by the message fields.
// Integers are little-endian, strings are a u16 byte length followed by UTF-8 bytes.
//
// Handshake: the client sends Hello; the server answers with Welcome or with Reject and
// then drops the connection. The layout of Hello's first fields (magic, version) and of
// Reject must never change: that is what lets an old client and a new server tell each
// other that their versions differ.
namespace NetProtocol {

  // bump on every incompatible change of any message
  constexpr uint16_t Version = 6;
  // "OGMP", identifies OpenGothic multiplayer traffic
  constexpr uint32_t Magic   = 0x504D474F;

  // players in one session, the host included
  constexpr size_t   MaxPlayers     = 8;

  constexpr size_t   MaxNameLength  = 32;
  constexpr size_t   MaxWorldLength = 64;
  constexpr size_t   MaxChatLength  = 512;
  constexpr size_t   MaxReasonLength = 256;

  enum class MsgType : uint8_t {
    Hello   = 1,
    Welcome = 2,
    Reject  = 3,
    Chat    = 4,
    PlayerJoined = 5,
    PlayerLeft   = 6,
    PlayerSpawn  = 7,
    PlayerState  = 8,
    WorldTime    = 9,
    };

  enum class RejectReason : uint8_t {
    VersionMismatch = 1,
    BadName         = 2,
    ServerFull      = 3,
    Malformed       = 4,
    };

  // client -> server, first message after connecting
  struct Hello {
    uint16_t    version = Version;
    std::string name;
    };

  // server -> client, handshake accepted
  struct Welcome {
    uint32_t    playerId   = 0;
    std::string worldName;
    uint64_t    serverTick = 0;
    };

  // server -> client, handshake refused; the server disconnects right after
  struct Reject {
    RejectReason reason        = RejectReason::Malformed;
    uint16_t     serverVersion = Version;
    std::string  text;
    };

  // client -> server: playerId is ignored, the server fills it in and relays to everyone
  struct Chat {
    uint32_t    playerId = 0;
    std::string text;
    };

  // server -> client: a player is in the session; right before Welcome the server sends one
  // for every player already there (the host included), later one for every newcomer
  struct PlayerJoined {
    uint32_t    playerId = 0;
    std::string name;
    };

  // server -> client: a player has left the session
  struct PlayerLeft {
    uint32_t    playerId = 0;
    };

  // server -> client: the character of a player is in the server's world under network id
  // entityId (see NetEntityId), at position x,y,z turned by rotation (degrees, Npc::rotation()).
  // Sent when a character is spawned or respawned with a new id, and to a newcomer right after
  // Welcome for every character already there. For its own player the client only takes over the id.
  struct PlayerSpawn {
    uint32_t    playerId = 0;
    uint32_t    entityId = 0;
    float       x = 0, y = 0, z = 0;
    float       rotation = 0;
    };

  // player -> server -> other players, over the unreliable channel ~20 times a second: where a
  // player's own character is and what it is doing. Each player moves its own character;
  // the server relays the states (with playerId filled in) and applies them to its world.
  // seq grows with every state of one player, so a late packet can be told from a newer one;
  // time is the sender's session clock in ms, for interpolation (MP-11).
  struct PlayerState {
    uint32_t    playerId    = 0;
    uint32_t    entityId    = 0;   // character the state belongs to (PlayerSpawn::entityId)
    uint32_t    seq         = 0;
    uint32_t    time        = 0;
    float       x = 0, y = 0, z = 0;
    float       rotation    = 0;   // degrees, Npc::rotation()
    uint32_t    bodyState   = 0;   // BodyState, Npc::bodyStateMasked()
    uint16_t    anim        = 0;   // AnimationSolver::Anim last started by the character
    uint8_t     walkMode    = 0;   // WalkBit
    uint8_t     weaponState = 0;   // WeaponState
    uint32_t    meleeWeapon  = 0;  // script symbol of the equipped melee weapon (Item::clsId()), 0: none
    uint32_t    rangedWeapon = 0;  // script symbol of the equipped bow or crossbow, 0: none
    };

  // server -> client, about once a second and after every jump of the clock: the time of day in
  // the server's world (gtime::toInt(), game milliseconds since day 0, 0:00). Clients keep their
  // clock running in between but always take this one over (MP-12).
  struct WorldTime {
    int64_t     time = 0;      // never negative
    };

  using Message = std::variant<Hello,Welcome,Reject,Chat,PlayerJoined,PlayerLeft,PlayerSpawn,PlayerState,WorldTime>;

  std::vector<uint8_t> encode(const Message& msg);
  // Returns nothing for truncated, oversized or unknown packets.
  // A Hello with a foreign version decodes to just its version (the rest of its layout is unknown).
  std::optional<Message> decode(const uint8_t* data, size_t size);

  // Server side: returns the Reject to send back, or nothing when the Hello is acceptable.
  std::optional<Reject> checkHello(const Hello& hello);
  // Human-readable text of a rejection, for the client to show to the player.
  std::string           describe(const Reject& reject, uint16_t clientVersion = Version);
  }
