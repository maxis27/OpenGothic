#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>

#include "netprotocol.h"
#include "nettransport.h"

// Multiplayer session on top of NetTransport and NetProtocol: the handshake, the list of
// players and chat. The host is a player too (id HostPlayer) and relays chat between clients.
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
    // Name of a player in the session, empty when unknown.
    auto     playerName(PlayerId id) const -> std::string_view;

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

    void     send       (NetTransport::PeerId peer, const NetProtocol::Message& msg);
    void     sendOthers (NetTransport::PeerId except, const NetProtocol::Message& msg);
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
    // host: player of each connected peer, NoPlayer until its Hello is accepted
    std::unordered_map<NetTransport::PeerId,PlayerId> peers;
    PlayerId                                         nextPlayer = HostPlayer+1;
    // client: the connection to the host
    NetTransport::PeerId                             hostPeer = NetTransport::InvalidPeer;
  };
