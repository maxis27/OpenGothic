// Two-process NetTransport test:
//   NetTransportTest host <port>
//   NetTransportTest connect <address> <port>
// The host echoes every packet back on the channel it came from. The client sends a batch of
// reliable packets (one of them large enough to be fragmented) and unreliable packets, checks the
// echoes and disconnects. Both processes exit with 0 on success.

#include "net/nettransport.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

constexpr uint32_t ReliableCount   = 100;
constexpr uint32_t UnreliableCount = 50;
constexpr size_t   LargeSize       = 64*1024;
constexpr auto     Timeout         = std::chrono::seconds(15);

std::vector<uint8_t> makePacket(uint32_t index, size_t size) {
  std::vector<uint8_t> pkg(size<sizeof(index) ? sizeof(index) : size);
  std::memcpy(pkg.data(), &index, sizeof(index));
  for(size_t i=sizeof(index); i<pkg.size(); ++i)
    pkg[i] = uint8_t(i*31u + index);
  return pkg;
  }

bool checkPacket(const NetTransport::Event& e, uint32_t index, size_t size) {
  auto expect = makePacket(index, size);
  return e.size==expect.size() && std::memcmp(e.data, expect.data(), e.size)==0;
  }

uint32_t packetIndex(const NetTransport::Event& e) {
  uint32_t index = 0;
  if(e.size>=sizeof(index))
    std::memcpy(&index, e.data, sizeof(index));
  return index;
  }

size_t packetSize(uint32_t index) {
  return index==ReliableCount/2 ? LargeSize : 16 + index;
  }

int runHost(uint16_t port) {
  NetTransport net;
  if(!net.host(port, 4)) {
    std::fprintf(stderr, "host: unable to listen on port %u\n", unsigned(port));
    return 1;
    }
  std::fprintf(stderr, "host: listening on port %u\n", unsigned(port));

  bool     connected = false;
  bool     done      = false;
  uint32_t echoed    = 0;
  auto     deadline  = Clock::now() + Timeout;
  while(!done && Clock::now()<deadline) {
    net.poll([&](const NetTransport::Event& e) {
      switch(e.type) {
        case NetTransport::EventType::Connect:
          std::fprintf(stderr, "host: peer %u connected\n", unsigned(e.peer));
          connected = true;
          break;
        case NetTransport::EventType::Receive:
          net.send(e.peer, e.channel, e.data, e.size);
          ++echoed;
          break;
        case NetTransport::EventType::Disconnect:
          std::fprintf(stderr, "host: peer %u disconnected\n", unsigned(e.peer));
          done = true;
          break;
        }
      }, 10);
    }
  net.flush();
  std::fprintf(stderr, "host: echoed %u packets\n", unsigned(echoed));

  if(!connected || !done || echoed<ReliableCount) {
    std::fprintf(stderr, "host: FAILED (connected=%d, disconnected=%d, echoed=%u)\n", connected, done, unsigned(echoed));
    return 1;
    }
  return 0;
  }

int runClient(const std::string& address, uint16_t port) {
  NetTransport net;
  // the host process may still be starting, so retry the connection a few times
  NetTransport::PeerId server    = NetTransport::InvalidPeer;
  bool                 connected = false;
  auto                 deadline  = Clock::now() + Timeout;
  while(!connected && Clock::now()<deadline) {
    server = net.connect(address, port);
    if(server==NetTransport::InvalidPeer) {
      std::fprintf(stderr, "client: unable to resolve %s\n", address.c_str());
      return 1;
      }
    bool failed = false;
    auto retryAt = Clock::now() + std::chrono::seconds(2);
    while(!connected && !failed && Clock::now()<retryAt) {
      net.poll([&](const NetTransport::Event& e) {
        if(e.type==NetTransport::EventType::Connect)
          connected = true;
        if(e.type==NetTransport::EventType::Disconnect)
          failed = true;
        }, 10);
      }
    }
  if(!connected) {
    std::fprintf(stderr, "client: unable to connect to %s:%u\n", address.c_str(), unsigned(port));
    return 1;
    }
  std::printf("client: connected to %s:%u\n", address.c_str(), unsigned(port));

  for(uint32_t i=0; i<ReliableCount; ++i) {
    auto pkg = makePacket(i, packetSize(i));
    if(!net.send(server, NetTransport::Reliable, pkg.data(), pkg.size())) {
      std::fprintf(stderr, "client: send failed\n");
      return 1;
      }
    }
  for(uint32_t i=0; i<UnreliableCount; ++i) {
    auto pkg = makePacket(ReliableCount+i, 8);
    net.send(server, NetTransport::Unreliable, pkg.data(), pkg.size());
    }
  net.flush();

  uint32_t nextReliable = 0;
  uint32_t unreliable   = 0;
  bool     ok           = true;
  bool     lost         = false;
  deadline = Clock::now() + Timeout;
  while(ok && !lost && nextReliable<ReliableCount && Clock::now()<deadline) {
    net.poll([&](const NetTransport::Event& e) {
      if(e.type==NetTransport::EventType::Disconnect) {
        lost = true;
        return;
        }
      if(e.type!=NetTransport::EventType::Receive)
        return;
      if(e.channel==NetTransport::Unreliable) {
        uint32_t idx = packetIndex(e);
        if(idx<ReliableCount || !checkPacket(e, idx, 8)) {
          std::fprintf(stderr, "client: bad unreliable echo %u\n", unsigned(idx));
          ok = false;
          }
        ++unreliable;
        return;
        }
      if(!checkPacket(e, nextReliable, packetSize(nextReliable))) {
        std::fprintf(stderr, "client: reliable echo %u is out of order or corrupted\n", unsigned(nextReliable));
        ok = false;
        return;
        }
      ++nextReliable;
      }, 10);
    }
  std::printf("client: %u/%u reliable, %u/%u unreliable echoes\n",
              unsigned(nextReliable), unsigned(ReliableCount), unsigned(unreliable), unsigned(UnreliableCount));

  // graceful shutdown, so the host sees a Disconnect event
  net.disconnect(server);
  auto closeAt = Clock::now() + std::chrono::milliseconds(500);
  while(Clock::now()<closeAt)
    net.poll([](const NetTransport::Event&){}, 10);
  net.close();

  if(!ok || lost || nextReliable!=ReliableCount) {
    std::fprintf(stderr, "client: FAILED\n");
    return 1;
    }
  return 0;
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
  if(argc==3 && std::strcmp(argv[1],"host")==0 && parsePort(argv[2],port))
    return runHost(port);
  if(argc==4 && std::strcmp(argv[1],"connect")==0 && parsePort(argv[3],port))
    return runClient(argv[2], port);
  std::fprintf(stderr, "usage: %s host <port> | connect <address> <port>\n", argv[0]);
  return 2;
  }
