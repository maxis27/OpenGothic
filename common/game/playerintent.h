#pragma once

#include <cstdint>

/// What a player wants its character to do: movement axes, rotation and actions.
/// PlayerControl fills it from keyboard and mouse; a network peer can fill it from packets.
/// PlayerMovement consumes it and drives the Npc, so the same code moves local and remote players.
struct PlayerIntent final {
  enum WeaponAction : uint8_t {
    WeaponClose,
    WeaponMele,
    WeaponBow,
    Weapon3,
    Weapon4,
    Weapon5,
    Weapon6,
    Weapon7,
    Weapon8,
    Weapon9,
    Weapon10,

    Last,
    };

  enum CombatAction : uint8_t {
    ActForward=0,
    ActBack   =1,
    ActLeft   =2,
    ActRight  =3,
    ActGeneric=4,
    ActMove   =5,
    ActKill   =6,

    CombatLast
    };

  /// Movement axes, each -1, 0 or 1
  float        forward       = 0; //!< >0 forward, <0 backward
  float        strafe        = 0; //!< >0 right,   <0 left
  float        turn          = 0; //!< >0 right,   <0 left

  /// Accumulated mouse rotation, consumed by PlayerMovement
  float        rotMouse      = 0;
  float        rotMouseY     = 0;

  /// Held keys that movement code reads directly
  bool         actionGeneric = false;
  bool         forwardKey    = false;
  bool         backKey       = false;
  bool         jump          = false;
  bool         transformBack = false;

  /// Pending weapon switch requests, cleared when fulfilled
  bool         weapon[WeaponAction::Last] = {};
  WeaponAction weaponLast    = WeaponMele; //!< Reminder for weapon toggle.

  /// Pending combat actions (attacks, parade, focus switch)
  bool         combat[CombatLast] = {};

  /// Set by PlayerMovement when it cancels movement; the input source must drop the held keys.
  bool         stopMove      = false; //!< all axes
  bool         stopStrafe    = false; //!< strafe axis only

  bool wantsToMoveForward () const { return forward>0.f; }
  bool wantsToMoveBackward() const { return forward<0.f; }
  bool wantsToStrafeRight () const { return strafe >0.f; }
  bool wantsToStrafeLeft  () const { return strafe <0.f; }
  bool wantsToTurnRight   () const { return turn   >0.f; }
  bool wantsToTurnLeft    () const { return turn   <0.f; }

  void clearCombat() {
    for(auto& c:combat)
      c = false;
    }
  void clearWeapon() {
    for(auto& w:weapon)
      w = false;
    }
  void cancelMovement() {
    forward  = 0;
    strafe   = 0;
    turn     = 0;
    stopMove = true;
    }
  void cancelStrafe() {
    strafe     = 0;
    stopStrafe = true;
    }
  };
