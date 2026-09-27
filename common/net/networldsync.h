#pragma once

#include <cstdint>

class NetSession;
class World;

// Keeps the player characters of a World in line with a NetSession (listen server):
//  - host: spawns a character for every other player at the world's start point and
//    announces the network id of every player's character, its own hero included;
//  - client: takes the ids over from the host: its own hero gets its id, the other players'
//    characters are spawned where the host has them;
//  - every player sends the state of its own character (NetSession::sendPlayerState) and
//    plays back the states received for the other players' characters (NetInterpolator);
//  - every player sends the attacks of its own character; the others replay them on their copy
//    of it as the playback of its movement reaches them. Only the host deals damage: it sends
//    every hit, the clients play its effects and take the hit points over (MP-15);
//  - a shot is fired again by the copies of the shooter, at the target where each world has it; only the
//    arrow of the host's copy deals damage, like a blow (MP-17);
//  - a spell is drawn, charged and cast again by the copies of the caster: its projectile at the target where
//    each world has it, its other effects on the target; only the host's copy deals damage and the host's Hit
//    carries the spell, for its effect on the target. The spell's scripts (mana, summons, transformations)
//    run only for the caster's own character (MP-18);
//  - a character falls dead or unconscious as the host's Hit or its own player says, gets up when its
//    player's does; the host respawns a dead one at the start point after RespawnDelayMs (MP-16);
//  - the host sends the npcs of its world, the clients have only these: they create none of their own, not even
//    the ones of the world's startup scripts (World::addNpc); an npc the host no longer has is removed (MP-19).
//    They run no AI (NetProxy): the host sends what its npcs near each player do (NpcStates: position, walk
//    mode, weapon, the animations playing, so routines as much as walks and blows), changes only, and the
//    clients play it back like the players' characters (MP-20);
//  - the items on the ground are the host's too (MP-22): the clients have only the ones it spawns, none of their own
//    (World::addItem). A player picking one up asks the host, which grants it to the first one only and despawns it
//    for everyone; until then the item is held aside. What a player drops (a weapon falling from its hand too) is
//    put down by the host where the player dropped it. Using an item (eating, drinking) is sent as its animation
//    starts; the item's effect runs only for the player's own character, its copies take the change of hit points
//    over. The copies play the animations of all of it at the moment of the player's movement it was done in;
//  - the host sends the time of day of its world, the clients take it over (MP-12);
//  - characters of players who left are removed, all of them once the session is gone.
// Called every frame. A newly loaded world has no remote players yet and is filled again.
namespace NetWorldSync {
  // how long a player's character lies dead before the host brings it back
  constexpr uint64_t RespawnDelayMs = 5000;
  // how far from another player's character (on the host, which sees it a little late) an item it takes or drops can
  // be, cm
  constexpr float    ItemReach      = 1000.f;
  void tick(NetSession* session, World& world);
  }
