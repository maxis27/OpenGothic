#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

#include "netprotocol.h"
#include "nettransport.h"

// Multiplayer session on top of NetTransport and NetProtocol: the handshake, the list of
// players, their characters in the host's world, their movement and chat. The host is a player too
// (id HostPlayer) and relays chat between clients.
// Independent of the game itself; poll() must be called regularly (every frame) from one thread.
class NetSession final {
  public:
    using PlayerId = uint32_t;
    static constexpr PlayerId NoPlayer   = 0;
    static constexpr PlayerId HostPlayer = 1;

    enum class State : uint8_t {
      Connecting, // client: waiting for the connection or for Welcome
      Online,
      Closed,     // rejected, disconnected or failed; the session can be dropped
      };

    NetSession(const NetSession&) = delete;
    NetSession& operator = (const NetSession&) = delete;
    ~NetSession();

    // Returns nullptr when the port can't be opened.
    static std::unique_ptr<NetSession> host   (uint16_t port, std::string_view name, std::string_view world);
    // Returns nullptr when connecting can't even start (e.g. unknown host name).
    static std::unique_ptr<NetSession> connect(std::string_view hostName, uint16_t port, std::string_view name);

    bool     isHost()   const { return server; }
    State    state()    const { return st; }
    PlayerId playerId() const { return self; }
    size_t   playerCount() const { return players.size(); }
    // Players in the session by id, this one included.
    auto     playerList() const -> const std::map<PlayerId,std::string>& { return players; }
    // Name of a player in the session, empty when unknown.
    auto     playerName(PlayerId id) const -> std::string_view;

    // Character of a player in the host's world, nullptr while the host hasn't spawned it.
    using Avatar = NetProtocol::PlayerSpawn;
    auto     avatar(PlayerId id) const -> const Avatar*;
    // Host: the character of a player in the session is in the world. The clients are told
    // when it is new or has another entity id than before; newcomers get all of them right
    // after Welcome. Ignored on a client and for players not in the session.
    void     setAvatar(const Avatar& a);

    // Movement of the players' characters. Every player moves its own character and sends
    // its state; the host relays the states of each player to the others.
    using PlayerState = NetProtocol::PlayerState;
    // at most this often a player's state is sent (20 Hz)
    static constexpr uint64_t StateIntervalMs = 50;
    // Sends the state of this player's character (playerId, seq and time are filled in here)
    // when StateIntervalMs have passed since the last one; false when not sent.
    bool     sendPlayerState(const PlayerState& s);
    // Latest state received from another player, nullptr when none yet.
    auto     playerState(PlayerId id) const -> const PlayerState*;

    // Time of day in the host's world (gtime::toInt()): the host owns the clock, the clients follow.
    // at most this often the host sends its time while the clock runs normally
    static constexpr uint64_t WorldTimeIntervalMs = 1000;
    // a clock this far off the last sent time (game ms) has jumped, e.g. by sleeping: sent at once
    static constexpr int64_t  WorldTimeJump       = 60*60*1000;
    // Host: the current time of its world, called every frame. Sent to the clients every
    // WorldTimeIntervalMs or at once when it has jumped; newcomers get it right after Welcome.
    // Ignored on a client and for negative times.
    void     setWorldTime(int64_t time);
    // Client: the host's time received since the last call, nothing when none came.
    auto     takeWorldTime() -> std::optional<int64_t>;

    // Service the network: handshake, chat, players joining and leaving.
    void     poll(uint32_t timeoutMs = 0);
    // Send a chat line to the other players; false when offline or the text is empty.
    // The line is also reported through onMessage, like the ones from other players.
    bool     sendChat(std::string_view text);

    // Chat lines and session notices ("X joined", rejections, ...) to show to the player.
    std::function<void(std::string_view)> onMessage;

  private:
    NetSession(bool server, std::string_view name);

    void     hostEvent  (const NetTransport::Event& e);
    void     clientEvent(const NetTransport::Event& e);
    void     onHello    (NetTransport::PeerId peer, const NetProtocol::Hello& hello);
    void     reject     (NetTransport::PeerId peer, const NetProtocol::Reject& r);

    void     onPlayerState(PlayerId from, NetProtocol::PlayerState s, NetTransport::PeerId peer);
    void     forgetPlayer (PlayerId id);

    void     send       (NetTransport::PeerId peer, const NetProtocol::Message& msg,
                         NetTransport::Channel ch = NetTransport::Reliable);
    void     sendOthers (NetTransport::PeerId except, const NetProtocol::Message& msg,
                         NetTransport::Channel ch = NetTransport::Reliable);
    void     notify     (const std::string& text);
    void     chatLine   (PlayerId from, std::string_view text);

    NetTransport                                     net;
    const bool                                       server = false;
    State                                            st     = State::Connecting;
    PlayerId                                         self   = NoPlayer;
    std::string                                      name;
    std::string                                      world;
    uint64_t                                         startTime = 0;

    std::map<PlayerId,std::string>                   players;
    std::map<PlayerId,Avatar>                        avatars;
    std::map<PlayerId,PlayerState>                   states;
    uint32_t                                         stateSeq  = 0;
    uint64_t                                         stateSent = 0;
    // host: last time of its world set and last one sent (with when); client: the host's time not taken yet
    std::optional<int64_t>                           worldTime;
    std::optional<int64_t>                           worldTimeSent;
    uint64_t                                         worldTimeSentAt = 0;
    // host: player of each connected peer, NoPlayer until its Hello is accepted
    std::unordered_map<NetTransport::PeerId,PlayerId> peers;
    PlayerId                                         nextPlayer = HostPlayer+1;
    // client: the connection to the host
    NetTransport::PeerId                             hostPeer = NetTransport::InvalidPeer;
  };
