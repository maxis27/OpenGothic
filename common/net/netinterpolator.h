#pragma once

#include <cstdint>
#include <deque>

#include "netprotocol.h"

// Smooth movement of a character replicated from the network. Keeps the recent states of it
// and plays them back Delay ms behind the newest one, interpolating position and rotation
// between the two states around that moment, so late or lost packets don't make it jerk.
// The sender's clock (PlayerState::time) is mapped to the local one from the arrival times.
// Independent of the game itself; times are in ms of any monotonic local clock.
// S is a state with seq, time, x, y, z and rotation: PlayerState, or NpcState for the npcs of the host (MP-20).
template<class S>
class NetInterpolatorT final {
  public:
    using State = S;

    // how far behind the newest state the character is shown
    static constexpr uint32_t Delay        = 100;
    // states are kept for this long (sender's clock)
    static constexpr uint32_t History      = 1000;
    // two states farther apart than this are a teleport: no sliding in between
    static constexpr float    SnapDistance = 500.f;

    struct Sample {
      float        x = 0, y = 0, z = 0;
      float        rotation = 0;       // degrees
      const State* state    = nullptr; // latest state at the sampled moment: animation, walk mode, ...
      };

    // A state received at local time now. States older than the newest one are ignored.
    void push(const State& s, uint64_t now);
    // The character at local time now, shifted by Delay; false while nothing was pushed.
    bool sample(uint64_t now, Sample& out) const;
    // The moment played back at local time now, on the sender's clock (PlayerState::time):
    // an event the sender stamped with a time up to this is due. False while nothing was pushed.
    bool playbackTime(uint64_t now, int64_t& out) const;
    void clear();

    bool   empty() const { return states.empty(); }
    size_t size()  const { return states.size();  }
    // newest state pushed, nullptr when none
    const State* newest() const { return states.empty() ? nullptr : &states.back(); }

  private:
    std::deque<State> states;
    // local time minus sender's time, of the fastest packet (slowly following drift)
    int64_t           offset    = 0;
  };

// the players' characters
using NetInterpolator    = NetInterpolatorT<NetProtocol::PlayerState>;
// the npcs of the host's world on a client
using NetNpcInterpolator = NetInterpolatorT<NetProtocol::NpcState>;

extern template class NetInterpolatorT<NetProtocol::PlayerState>;
extern template class NetInterpolatorT<NetProtocol::NpcState>;
