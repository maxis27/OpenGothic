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
  constexpr uint16_t Version = 8;
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
    PlayerAttack = 10,
    Hit          = 11,
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
  // Welcome for every character already there. For its own player the client only takes over the id,
  // unless the character is respawned (MP-16).
  struct PlayerSpawn {
    enum Flag : uint8_t {
      Respawn  = 1<<0, // the character was dead and is back to life here, with full hit points and a new id;
                       // never set in the ones a newcomer gets
      AllFlags = (1<<1)-1,
      };
    uint32_t    playerId = 0;
    uint32_t    entityId = 0;
    float       x = 0, y = 0, z = 0;
    float       rotation = 0;
    uint8_t     flags    = 0;   // Flag
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

  // melee moves of PlayerAttack, the combat actions of PlayerIntent that start one
  enum class AttackMove : uint8_t {
    Swing      = 1, // ActForward: fists or weapon forward, continuing a combo
    SwingLeft  = 2, // ActLeft
    SwingRight = 3, // ActRight
    Parade     = 4, // ActBack
    Finish     = 5, // ActKill: finishing move on an unconscious character
    };

  // player -> server -> other players, reliable: a player's character has started an attack.
  // The others replay it on their copy of the character when their playback of its states
  // (NetInterpolator) reaches time; only the host's copy deals damage, see Hit.
  struct PlayerAttack {
    uint32_t    playerId = 0;
    uint32_t    entityId = 0;   // attacking character (PlayerSpawn::entityId)
    uint32_t    time     = 0;   // sender's session clock in ms, the clock of PlayerState::time
    uint32_t    target   = 0;   // entity id of the character aimed at, 0: none
    AttackMove  move     = AttackMove::Swing;
    };

  // server -> client, reliable: a character with a network id was hit in the server's world.
  // The server alone deals damage; a client plays the effects and takes the hit points over.
  struct Hit {
    enum Flag : uint8_t {
      Effect   = 1<<0, // the weapon's hit effect (sound, blood)
      Blocked  = 1<<1, // parried: block effect, no damage
      Stumble  = 1<<2, // the target is thrown back (stumble animation)
      StumbleB = 1<<3, // with the StumbleB animation instead of StumbleA
      DontKill = 1<<4, // at no hit points the target falls unconscious instead of dying
      Scream   = 1<<5, // the target cries out
      // the state of the target after the hit (MP-16), at most one of them
      Dead        = 1<<6, // dead
      Unconscious = 1<<7, // fallen unconscious
      AllFlags    = 0xFF,
      };
    uint32_t    attacker = 0;   // entity id, 0: none or unknown to the network
    uint32_t    target   = 0;   // entity id, never 0
    int32_t     hp       = 0;   // hit points of the target after the hit, >= 0
    int32_t     damage   = 0;   // hit points taken, >= 0
    uint8_t     flags    = 0;   // Flag
    };

  using Message = std::variant<Hello,Welcome,Reject,Chat,PlayerJoined,PlayerLeft,PlayerSpawn,PlayerState,WorldTime,
                               PlayerAttack,Hit>;

  std::vector<uint8_t> encode(const Message& msg);
  // Returns nothing for truncated, oversized or unknown packets.
  // A Hello with a foreign version decodes to just its version (the rest of its layout is unknown).
  std::optional<Message> decode(const uint8_t* data, size_t size);

  // Server side: returns the Reject to send back, or nothing when the Hello is acceptable.
  std::optional<Reject> checkHello(const Hello& hello);
  // Human-readable text of a rejection, for the client to show to the player.
  std::string           describe(const Reject& reject, uint16_t clientVersion = Version);
  }
