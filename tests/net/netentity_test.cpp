// NetEntity test: NetEntityId <-> Npc*/Item* registry.
// Usage: NetEntityTest. Exits with 0 on success.

#include "net/netentity.h"

#include <cstdio>

// the registry only stores pointers, stand-ins for the game classes are enough
class Npc  { public: int dummy = 0; };
class Item { public: int dummy = 0; };

namespace {

int failures = 0;

void check(bool cond, const char* what) {
  if(cond)
    return;
  std::fprintf(stderr, "FAILED: %s\n", what);
  ++failures;
  }

void testAdd() {
  NetEntityRegistry reg;
  Npc  a, b;
  Item i;

  auto ia = reg.add(a);
  auto ib = reg.add(b);
  auto ii = reg.add(i);
  check(bool(ia) && bool(ib) && bool(ii),     "ids are not 0");
  check(ia!=ib && ia!=ii && ib!=ii,           "ids are unique across npcs and items");
  check(reg.add(a)==ia,                       "adding twice keeps the id");
  check(reg.size()==3,                        "size after add");
  check(reg.npc(ia)==&a && reg.npc(ib)==&b,   "npc lookup by id");
  check(reg.item(ii)==&i,                     "item lookup by id");
  check(reg.id(a)==ia && reg.id(i)==ii,       "id lookup by pointer");
  check(reg.item(ia)==nullptr,                "npc id is not an item");
  check(reg.npc(ii)==nullptr,                 "item id is not an npc");
  check(reg.kind(ia)==NetEntityRegistry::Kind::Npc,  "kind of npc");
  check(reg.kind(ii)==NetEntityRegistry::Kind::Item, "kind of item");
  check(reg.kind(NetEntityId{999})==NetEntityRegistry::Kind::None, "kind of unknown id");
  }

void testRemove() {
  NetEntityRegistry reg;
  Npc  a, b;
  Item i;
  auto ia = reg.add(a);
  auto ib = reg.add(b);
  auto ii = reg.add(i);

  reg.remove(a);
  check(reg.npc(ia)==nullptr && !reg.id(a), "removed npc is gone");
  check(reg.npc(ib)==&b,                    "other npc stays");
  reg.remove(ii);
  check(reg.item(ii)==nullptr && !reg.id(i), "item removed by id is gone");
  reg.remove(a);
  reg.remove(NetEntityId{12345});
  check(reg.size()==1,                      "removing unknown entries is a no-op");

  // ids are never reused within one registry: a stale id must not point at a new object
  Npc c;
  auto ic = reg.add(c);
  check(ic!=ia && ic!=ib && ic!=ii,         "removed ids are not reused");
  check(reg.add(a)!=ia,                     "re-added npc gets a new id");

  reg.clear();
  check(reg.size()==0 && reg.npc(ib)==nullptr && !reg.id(c), "clear empties the registry");
  }

void testBind() {
  NetEntityRegistry reg;
  Npc  a, b;
  Item i;

  check(reg.bind(NetEntityId{7}, a),        "bind npc");
  check(reg.npc(NetEntityId{7})==&a,        "bound npc lookup");
  check(reg.bind(NetEntityId{7}, a),        "binding the same pair again is accepted");
  check(!reg.bind(NetEntityId{7}, b),       "id taken by another npc");
  check(!reg.bind(NetEntityId{8}, a),       "npc already has another id");
  check(!reg.bind(NetEntityId{7}, i),       "id taken by an npc cannot become an item");
  check(!reg.bind(NetEntityId{}, b),        "id 0 is refused");
  check(reg.bind(NetEntityId{1}, i),        "bind item");
  check(reg.item(NetEntityId{1})==&i,       "bound item lookup");

  // local allocation skips ids that came from the host
  auto ib = reg.add(b);
  check(ib!=NetEntityId{1} && ib!=NetEntityId{7} && bool(ib), "add skips bound ids");
  }

void testAutoAssign() {
  NetEntityRegistry reg;
  check(!reg.autoAssign(), "auto assign is off by default");
  reg.setAutoAssign(true);
  check(reg.autoAssign(),  "auto assign can be enabled");
  }

}

int main() {
  testAdd();
  testRemove();
  testBind();
  testAutoAssign();
  if(failures>0) {
    std::fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
    }
  std::printf("NetEntity test passed\n");
  return 0;
  }
