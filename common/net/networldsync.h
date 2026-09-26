#pragma once

class NetSession;
class World;

// Keeps the player characters of a World in line with a NetSession (listen server):
//  - host: spawns a character for every other player at the world's start point and
//    announces the network id of every player's character, its own hero included;
//  - client: takes the ids over from the host: its own hero gets its id, the other players'
//    characters are spawned where the host has them;
//  - characters of players who left are removed, all of them once the session is gone.
// Called every frame. A newly loaded world has no remote players yet and is filled again.
namespace NetWorldSync {
  void tick(NetSession* session, World& world);
  }
