#include "nettransport.h"

#include <enet/enet.h>

#include <mutex>

namespace {

// enet_initialize/enet_deinitialize are global; keep them balanced across transports
std::mutex enetSync;
size_t     enetUsers = 0;

bool enetAcquire() {
  std::lock_guard<std::mutex> guard(enetSync);
  if(enetUsers==0 && enet_initialize()!=0)
    return false;
  ++enetUsers;
  return true;
  }

void enetRelease() {
  std::lock_guard<std::mutex> guard(enetSync);
  if(enetUsers==0)
    return;
  --enetUsers;
  if(enetUsers==0)
    enet_deinitialize();
  }

}

NetTransport::NetTransport() {
  }

NetTransport::~NetTransport() {
  close();
  }

bool NetTransport::host(uint16_t port, size_t maxPeers) {
  close();
  if(!enetAcquire())
    return false;

  ENetAddress addr = {};
  addr.host = ENET_HOST_ANY;
  addr.port = port;
  impl = enet_host_create(&addr, maxPeers, ChannelCount, 0, 0);
  if(impl==nullptr) {
    enetRelease();
    return false;
    }
  server = true;
  return true;
  }

NetTransport::PeerId NetTransport::connect(const std::string& hostName, uint16_t port) {
  close();
  if(!enetAcquire())
    return InvalidPeer;

  ENetAddress addr = {};
  if(enet_address_set_host(&addr, hostName.c_str())!=0) {
    enetRelease();
    return InvalidPeer;
    }
  addr.port = port;

  impl = enet_host_create(nullptr, 1, ChannelCount, 0, 0);
  if(impl==nullptr) {
    enetRelease();
    return InvalidPeer;
    }

  ENetPeer* p = enet_host_connect(impl, &addr, ChannelCount, 0);
  if(p==nullptr) {
    close();
    return InvalidPeer;
    }
  server = false;
  return addPeer(p);
  }

void NetTransport::close() {
  if(impl==nullptr)
    return;
  for(auto& [id,p]:peers) {
    p->data = nullptr;
    enet_peer_disconnect_now(p, 0);
    }
  peers.clear();
  enet_host_destroy(impl);
  impl   = nullptr;
  server = false;
  enetRelease();
  }

void NetTransport::poll(const std::function<void(const Event&)>& onEvent, uint32_t timeoutMs) {
  if(impl==nullptr)
    return;

  ENetEvent ev = {};
  while(impl!=nullptr && enet_host_service(impl, &ev, timeoutMs)>0) {
    timeoutMs = 0;

    Event e;
    switch(ev.type) {
      case ENET_EVENT_TYPE_CONNECT: {
        e.type = EventType::Connect;
        e.peer = peerId(ev.peer);
        if(e.peer==InvalidPeer)
          e.peer = addPeer(ev.peer);
        onEvent(e);
        break;
        }
      case ENET_EVENT_TYPE_DISCONNECT: {
        e.type = EventType::Disconnect;
        e.peer = peerId(ev.peer);
        if(e.peer==InvalidPeer)
          break; // already dropped by disconnect()
        peers.erase(e.peer);
        ev.peer->data = nullptr;
        onEvent(e);
        break;
        }
      case ENET_EVENT_TYPE_RECEIVE: {
        e.type    = EventType::Receive;
        e.peer    = peerId(ev.peer);
        e.channel = ev.channelID==Unreliable ? Unreliable : Reliable;
        e.data    = ev.packet->data;
        e.size    = ev.packet->dataLength;
        if(e.peer!=InvalidPeer)
          onEvent(e);
        enet_packet_destroy(ev.packet);
        break;
        }
      case ENET_EVENT_TYPE_NONE:
        break;
      }
    }
  }

void NetTransport::flush() {
  if(impl!=nullptr)
    enet_host_flush(impl);
  }

bool NetTransport::send(PeerId peer, Channel ch, const void* data, size_t size) {
  ENetPeer* p = findPeer(peer);
  if(p==nullptr || p->state!=ENET_PEER_STATE_CONNECTED)
    return false;

  const enet_uint32 flags = (ch==Reliable) ? ENET_PACKET_FLAG_RELIABLE : 0;
  ENetPacket* pkg = enet_packet_create(data, size, flags);
  if(pkg==nullptr)
    return false;
  if(enet_peer_send(p, ch, pkg)!=0) {
    enet_packet_destroy(pkg);
    return false;
    }
  return true;
  }

void NetTransport::broadcast(Channel ch, const void* data, size_t size) {
  for(auto& [id,p]:peers)
    send(id, ch, data, size);
  }

void NetTransport::disconnect(PeerId peer) {
  ENetPeer* p = findPeer(peer);
  if(p==nullptr)
    return;
  peers.erase(peer);
  p->data = nullptr;
  enet_peer_disconnect_later(p, 0);
  }

NetTransport::PeerId NetTransport::addPeer(ENetPeer* p) {
  const PeerId id = nextId++;
  if(nextId==InvalidPeer)
    nextId = 1;
  p->data = reinterpret_cast<void*>(uintptr_t(id));
  peers[id] = p;
  return id;
  }

NetTransport::PeerId NetTransport::peerId(const ENetPeer* p) const {
  return PeerId(reinterpret_cast<uintptr_t>(p->data));
  }

ENetPeer* NetTransport::findPeer(PeerId id) const {
  auto it = peers.find(id);
  if(it==peers.end())
    return nullptr;
  return it->second;
  }
