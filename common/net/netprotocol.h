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
  constexpr uint16_t Version = 1;
  // "OGMP", identifies OpenGothic multiplayer traffic
  constexpr uint32_t Magic   = 0x504D474F;

  constexpr size_t   MaxNameLength  = 32;
  constexpr size_t   MaxWorldLength = 64;
  constexpr size_t   MaxChatLength  = 512;
  constexpr size_t   MaxReasonLength = 256;

  enum class MsgType : uint8_t {
    Hello   = 1,
    Welcome = 2,
    Reject  = 3,
    Chat    = 4,
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

  using Message = std::variant<Hello,Welcome,Reject,Chat>;

  std::vector<uint8_t> encode(const Message& msg);
  // Returns nothing for truncated, oversized or unknown packets.
  // A Hello with a foreign version decodes to just its version (the rest of its layout is unknown).
  std::optional<Message> decode(const uint8_t* data, size_t size);

  // Server side: returns the Reject to send back, or nothing when the Hello is acceptable.
  std::optional<Reject> checkHello(const Hello& hello);
  // Human-readable text of a rejection, for the client to show to the player.
  std::string           describe(const Reject& reject, uint16_t clientVersion = Version);
  }
