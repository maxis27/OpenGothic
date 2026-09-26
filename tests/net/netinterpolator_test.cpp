// NetInterpolator test: playback of replicated states Delay ms late, with interpolation.
// Usage: NetInterpolatorTest. Exits with 0 on success.

#include "net/netinterpolator.h"

#include <cmath>
#include <cstdio>

namespace {

int failures = 0;

void check(bool cond, const char* what) {
  if(cond)
    return;
  std::fprintf(stderr, "FAILED: %s\n", what);
  ++failures;
  }

bool near(float a, float b) {
  return std::fabs(a-b)<0.01f;
  }

NetInterpolator::State state(uint32_t seq, uint32_t time, float x, float rotation = 0, uint16_t anim = 0) {
  NetInterpolator::State s;
  s.entityId = 1;
  s.seq      = seq;
  s.time     = time;
  s.x        = x;
  s.rotation = rotation;
  s.anim     = anim;
  return s;
  }

void testEmpty() {
  NetInterpolator in;
  NetInterpolator::Sample s;
  check(!in.sample(1000, s),                  "nothing to sample before the first state");
  check(in.newest()==nullptr,                 "no newest state");
  }

void testInterpolation() {
  NetInterpolator in;
  NetInterpolator::Sample s;
  // sender's clock at 5000, arrives at local 1000 (offset -4000)
  in.push(state(1, 5000, 0,   0, 2), 1000);
  in.push(state(2, 5050, 100, 0, 3), 1050);
  in.push(state(3, 5100, 200, 0, 4), 1100);

  check(in.sample(1100, s) && near(s.x, 0),   "Delay behind the newest state: the first one");
  check(in.sample(1125, s) && near(s.x, 50),  "halfway between two states");
  check(s.state!=nullptr && s.state->anim==2, "discrete fields of the earlier state");
  check(in.sample(1175, s) && near(s.x, 150), "halfway between the next two");
  check(s.state->anim==3,                     "animation switches with the states");
  check(in.sample(1400, s) && near(s.x, 200), "holds the newest state when nothing newer came");
  check(in.sample(900, s)  && near(s.x, 0),   "before the oldest state: the oldest one");
  }

void testLateAndDuplicate() {
  NetInterpolator in;
  in.push(state(1, 0,  0),  0);
  in.push(state(3, 100, 100), 100);
  in.push(state(2, 50, 999), 101);  // reordered, older than the newest
  in.push(state(3, 100, 999), 102); // the same state again
  check(in.size()==2,                         "late and repeated states are ignored");
  check(in.newest()->x==100.f,                "newest state kept");
  }

void testJitter() {
  NetInterpolator in;
  NetInterpolator::Sample s;
  // same sender timeline, the second packet was 30 ms late on the way
  in.push(state(1, 0,   0),   0);
  in.push(state(2, 50,  100), 80);
  in.push(state(3, 100, 200), 100);
  // the playback follows the sender's clock: the fastest packet decides the offset
  check(in.sample(125, s) && near(s.x, 50),   "late packet doesn't shift the playback");
  // a packet faster than all before moves the playback forward
  in.push(state(4, 150, 300), 140);
  check(in.sample(215, s) && near(s.x, 250),  "faster packet re-bases the clock");
  }

void testRotation() {
  NetInterpolator in;
  NetInterpolator::Sample s;
  in.push(state(1, 0,  0, 350), 0);
  in.push(state(2, 100, 0, 10), 100);
  check(in.sample(150, s),                    "sample");
  float r = std::fmod(s.rotation+360.f, 360.f);
  check(near(r, 0) || near(r, 360),           "rotation goes the short way across 0");
  }

void testSnap() {
  NetInterpolator in;
  NetInterpolator::Sample s;
  in.push(state(1, 0,   0),    0);
  in.push(state(2, 50,  5000), 50); // teleport
  check(in.sample(125, s) && near(s.x, 0),    "no sliding towards a teleport");
  check(in.sample(150, s) && near(s.x, 5000), "jumps there when its time comes");
  }

void testHistory() {
  NetInterpolator in;
  for(uint32_t i=1; i<=100; ++i)
    in.push(state(i, i*50, float(i)), i*50);
  check(in.size()<=NetInterpolator::History/50+2, "old states are dropped");
  NetInterpolator::Sample s;
  check(in.sample(5000, s) && near(s.x, 98),  "still plays back Delay behind");
  }

void testRestart() {
  NetInterpolator in;
  NetInterpolator::Sample s;
  in.push(state(1, 100000, 10), 1000);
  in.push(state(2, 20, 20), 2000); // sender's clock went back
  check(in.size()==1,                         "states of the old clock are dropped");
  check(in.sample(2000, s) && near(s.x, 20),  "plays the new ones");
  in.clear();
  check(in.empty(),                           "clear");
  }

}

int main() {
  testEmpty();
  testInterpolation();
  testLateAndDuplicate();
  testJitter();
  testRotation();
  testSnap();
  testHistory();
  testRestart();
  if(failures==0)
    std::printf("NetInterpolator: all tests passed\n");
  return failures==0 ? 0 : 1;
  }
