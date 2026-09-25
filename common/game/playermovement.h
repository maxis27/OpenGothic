#pragma once

#include "playerintent.h"

#include <cstdint>
#include <functional>
#include <optional>

class Npc;
class Interactive;

/// Drives a player-controlled Npc from a PlayerIntent: movement, rotation, weapon switching and attacks.
/// Knows nothing about keyboard, camera or UI; those come in through Env.
class PlayerMovement final {
  public:
    struct Env {
      bool                  g2Ctrl          = false;
      bool                  dialogActive    = false;
      bool                  printSpellName  = false;    //!< show spell description on draw (local player only)
      Interactive*          focus           = nullptr;  //!< focused mob, used to aim bow and spells
      std::optional<float>  cameraAzimuth;              //!< camera azimuth, used for run-angle
      std::function<void(PlayerIntent::CombatAction)> moveFocus; //!< switch focus to next npc left/right
      };

    void  tick(Npc& pl, PlayerIntent& in, const Env& env, uint64_t dt);

  private:
    bool     casting       = false;
    float    runAngleDest  = 0.f;
    uint64_t turnAniSmooth = 0;
    int      rotationAni   = 0;

    void     implMove        (Npc& pl, PlayerIntent& in, const Env& env, uint64_t dt);
    void     implMoveMobsi   (Npc& pl, PlayerIntent& in, uint64_t dt);
    void     moveFocus       (const Env& env, PlayerIntent::CombatAction act);
    void     assignRunAngle  (Npc& pl, const Env& env, uint64_t dt);
    void     setAnimRotate   (Npc& pl, float rotation, int anim, bool force, uint64_t dt);
    void     processAutoRotate(Npc& pl, const PlayerIntent& in, float& rot, uint64_t dt);
  };
