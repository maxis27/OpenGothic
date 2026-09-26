#include "netinterpolator.h"

#include <cmath>

namespace {

float lerp(float a, float b, float k) {
  return a + (b-a)*k;
  }

// along the shorter way around the circle, in degrees
float lerpAngle(float a, float b, float k) {
  float d = std::fmod(b-a, 360.f);
  if(d>180.f)
    d -= 360.f;
  if(d<-180.f)
    d += 360.f;
  return a + d*k;
  }

template<class State>
float distance(const State& a, const State& b) {
  const float dx = b.x-a.x, dy = b.y-a.y, dz = b.z-a.z;
  return std::sqrt(dx*dx + dy*dy + dz*dz);
  }

template<class Sample, class State>
void assign(Sample& out, const State& s) {
  out.x        = s.x;
  out.y        = s.y;
  out.z        = s.z;
  out.rotation = s.rotation;
  out.state    = &s;
  }

}

template<class S>
void NetInterpolatorT<S>::push(const State& s, uint64_t now) {
  if(!states.empty()) {
    auto& last = states.back();
    if(int32_t(s.seq - last.seq)<=0)
      return; // late packet
    if(s.time<last.time)
      clear(); // the sender's clock went back: a new session of it
    }

  const int64_t arrival = int64_t(now) - int64_t(s.time);
  if(states.empty() || arrival<offset)
    offset = arrival; // the fastest packet so far
  else
    offset += (arrival-offset)/32; // follow clock drift and a longer route slowly

  states.push_back(s);
  while(states.size()>2 && states.back().time-states[1].time>=History)
    states.pop_front();
  }

template<class S>
bool NetInterpolatorT<S>::playbackTime(uint64_t now, int64_t& out) const {
  if(states.empty())
    return false;
  out = int64_t(now) - offset - Delay;
  return true;
  }

template<class S>
bool NetInterpolatorT<S>::sample(uint64_t now, Sample& out) const {
  if(states.empty())
    return false;

  const int64_t t = int64_t(now) - offset - Delay; // on the sender's clock
  if(t<=int64_t(states.front().time)) {
    assign(out, states.front());
    return true;
    }

  for(size_t i=1; i<states.size(); ++i) {
    auto& b = states[i];
    if(t>=int64_t(b.time))
      continue;
    auto& a = states[i-1];
    if(b.time==a.time || distance(a,b)>SnapDistance) {
      assign(out, a);
      return true;
      }
    const float k = float(t-int64_t(a.time)) / float(b.time-a.time);
    out.x        = lerp(a.x, b.x, k);
    out.y        = lerp(a.y, b.y, k);
    out.z        = lerp(a.z, b.z, k);
    out.rotation = lerpAngle(a.rotation, b.rotation, k);
    out.state    = &a;
    return true;
    }

  // nothing newer arrived yet: hold the newest state
  assign(out, states.back());
  return true;
  }

template<class S>
void NetInterpolatorT<S>::clear() {
  states.clear();
  offset = 0;
  }

template class NetInterpolatorT<NetProtocol::PlayerState>;
template class NetInterpolatorT<NetProtocol::NpcState>;
