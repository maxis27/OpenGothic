#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>

struct _ENetHost;
struct _ENetPeer;

// Thin wrapper over ENet: one UDP host, either listening (server) or connecting (client).
// All calls must come from the same thread; events are delivered from poll().
class NetTransport final {
  public:
    using PeerId = uint32_t;
    static constexpr PeerId InvalidPeer = 0;

    enum Channel : uint8_t {
      Reliable     = 0, // ordered, resent until acknowledged
      Unreliable   = 1, // sequenced, may be dropped; for frequent state updates
      ChannelCount = 2,
      };

    enum class EventType : uint8_t {
      Connect,
      Disconnect,
      Receive,
      };

    struct Event {
      EventType      type    = EventType::Receive;
      PeerId         peer    = InvalidPeer;
      Channel        channel = Reliable;
      const uint8_t* data    = nullptr; // valid only inside the poll() callback
      size_t         size    = 0;
      };

    NetTransport();
    NetTransport(const NetTransport&) = delete;
    NetTransport& operator = (const NetTransport&) = delete;
    ~NetTransport();

    // Listen on the given UDP port for up to maxPeers clients.
    bool   host(uint16_t port, size_t maxPeers);
    // Start connecting to a server; completion is reported by a Connect event,
    // failure by a Disconnect event for the returned peer.
    PeerId connect(const std::string& hostName, uint16_t port);
    // Drop all peers (notifying them) and release the socket.
    void   close();

    bool   isOpen()   const { return impl!=nullptr; }
    bool   isServer() const { return server; }
    size_t peerCount() const { return peers.size(); }

    // Service the socket: send queued packets and dispatch received events.
    // Waits up to timeoutMs for the first event, then drains the rest without waiting.
    void   poll(const std::function<void(const Event&)>& onEvent, uint32_t timeoutMs = 0);
    // Send queued packets without waiting for poll().
    void   flush();

    bool   send     (PeerId peer, Channel ch, const void* data, size_t size);
    void   broadcast(Channel ch, const void* data, size_t size);
    void   disconnect(PeerId peer);

  private:
    PeerId     addPeer(_ENetPeer* p);
    PeerId     peerId (const _ENetPeer* p) const;
    _ENetPeer* findPeer(PeerId id) const;

    _ENetHost*                             impl   = nullptr;
    bool                                   server = false;
    PeerId                                 nextId = 1;
    std::unordered_map<PeerId,_ENetPeer*>  peers;
  };
