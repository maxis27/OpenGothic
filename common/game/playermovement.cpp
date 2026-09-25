#include "playermovement.h"

#include <cmath>

#include "world/objects/npc.h"
#include "world/objects/item.h"
#include "world/objects/interactive.h"
#include "world/world.h"
#include "utils/keycodec.h"
#include "gothic.h"

using Act = PlayerIntent::CombatAction;
using Wpn = PlayerIntent::WeaponAction;

void PlayerMovement::tick(Npc& pl, PlayerIntent& in, const Env& env, uint64_t dt) {
  const float dtF = float(dt)/1000.f;

  implMove(pl,in,env,dt);

  float runAngle = pl.runAngle();
  if(runAngle!=0.f || std::fabs(runAngleDest)>0.01f) {
    const float speed = 35.f;
    if(runAngle<runAngleDest) {
      runAngle+=speed*dtF;
      if(runAngle>runAngleDest)
        runAngle = runAngleDest;
      pl.setRunAngle(runAngle);
      }
    else if(runAngle>runAngleDest) {
      runAngle-=speed*dtF;
      if(runAngle<runAngleDest)
        runAngle = runAngleDest;
      pl.setRunAngle(runAngle);
      }
    }
  }

void PlayerMovement::moveFocus(const Env& env, PlayerIntent::CombatAction act) {
  if(env.moveFocus)
    env.moveFocus(act);
  }

void PlayerMovement::implMove(Npc& pl, PlayerIntent& in, const Env& env, uint64_t dt) {
  auto& w         = pl.world();
  float rot       = pl.rotation();
  float rotY      = pl.rotationY();
  // 100 / 200 according to some sources, yet my mesures are 90/180
  float rspeed    = (pl.weaponState()==WeaponState::NoWeapon ? 90.f : 180.f)*(float(dt)/1000.f);
  auto  ws        = pl.weaponState();
  auto  bs        = pl.bodyStateMasked();
  bool  allowRot  = !in.actionGeneric && pl.isRotationAllowed();
  auto& actrl     = in.combat;
  auto& wctrl     = in.weapon;

  Npc::Anim ani = Npc::Anim::Idle;

  if(bs==BS_DEAD)
    return;
  if(bs==BS_UNCONSCIOUS)
    return;

  if(!pl.isAiQueueEmpty()) {
    runAngleDest = 0;
    return;
    }

  if(pl.interactive()!=nullptr) {
    runAngleDest = 0;
    implMoveMobsi(pl,in,dt);
    return;
    }

  if(pl.canSwitchWeapon()) {
    if(wctrl[Wpn::WeaponClose]) {
      wctrl[Wpn::WeaponClose] = !(pl.closeWeapon(false) || pl.isMonster());
      return;
      }
    if(wctrl[Wpn::WeaponMele]) {
      bool ret=false;
      if(pl.currentMeleeWeapon()!=nullptr)
        ret = pl.drawWeaponMelee(); else
        ret = pl.drawWeaponFist();
      wctrl[Wpn::WeaponMele] = !ret;
      in.weaponLast          = Wpn::WeaponMele;
      if(!wctrl[Wpn::WeaponMele])
        return;
      }
    if(wctrl[Wpn::WeaponBow]) {
      if(pl.currentRangedWeapon()!=nullptr) {
        wctrl[Wpn::WeaponBow] = !pl.drawWeaponBow();
        in.weaponLast         = Wpn::WeaponBow;
        } else {
        wctrl[Wpn::WeaponBow] = false;
        }
      if(!wctrl[Wpn::WeaponBow])
        return;
      }
    for(uint8_t i=0;i<8;++i) {
      if(wctrl[Wpn::Weapon3+i]){
        if(pl.inventory().currentSpell(i)!=nullptr){
          bool ret = pl.drawMage(uint8_t(3+i));
          wctrl[Wpn::Weapon3+i] = !ret;
          in.weaponLast = static_cast<Wpn>(Wpn::Weapon3+i);
          if(ret && env.printSpellName) {
            if(auto spl = pl.inventory().currentSpell(i)) {
              Gothic::inst().onPrint(spl->description());
              }
            }
          } else {
          wctrl[Wpn::Weapon3+i] = false;
          return;
          }
        }
      }
    }

  if(!pl.isInState(ScriptFn()) || env.dialogActive) {
    runAngleDest = 0;
    return;
    }

  int rotation = 0;
  if(allowRot) {
    if(in.wantsToTurnLeft()) {
      rot += rspeed;
      rotation = -1;
      in.rotMouse=0;
      }
    if(in.wantsToTurnRight()) {
      rot -= rspeed;
      rotation = 1;
      in.rotMouse=0;
      }
    if(std::fabs(in.rotMouse)>0.f) {
      if(in.rotMouse>0)
        rotation = -1; else
        rotation = 1;
      rot += in.rotMouse;
      in.rotMouse  = 0;
      }
    rotY+=in.rotMouseY;
    } else {
    in.rotMouse  = 0;
    in.rotMouseY = 0;
    }

  pl.setDirectionY(rotY);
  if(pl.isFalling() || pl.isSlide() || pl.isInAir() || pl.isJump() || pl.isJumpUp()){
    pl.setDirection(rot);
    runAngleDest = 0;
    return;
    }

  if(casting) {
    if(!actrl[Act::ActForward] || (Gothic::inst().version().game==1 && pl.attribute(ATR_MANA)==0)) {
      casting = false;
      pl.endCastSpell(true);
      }
    return;
    }

  if(in.transformBack) {
    pl.transformBack();
    in.transformBack = false;
    }

  if((ws==WeaponState::Bow || ws==WeaponState::CBow) && pl.hasAmmunition()) {
    if(actrl[Act::ActGeneric] || actrl[Act::ActForward]) {
      if(auto other = pl.target()) {
        auto dp = other->position()-pl.position();
        pl.turnTo(dp.x,dp.z,true,dt);
        pl.aimBow();
        }
      else if(env.focus!=nullptr) {
        auto dp = env.focus->position()-pl.position();
        pl.turnTo(dp.x,dp.z,false,dt);
        pl.aimBow();
        }
      else {
        pl.aimBow();
        }

      if(actrl[Act::ActLeft]) {
        moveFocus(env,Act::ActLeft);
        actrl[Act::ActLeft]  = false;
        }
      if(actrl[Act::ActRight]) {
        moveFocus(env,Act::ActRight);
        actrl[Act::ActRight]  = false;
        }
      if(!actrl[Act::ActForward])
        return;
      }
    }

  if(ws==WeaponState::Mage) {
    if(actrl[Act::ActGeneric] || actrl[Act::ActForward] || in.actionGeneric) {
      if(auto other = pl.target()) {
        auto dp = other->centerPosition() - pl.centerPosition();
        pl.turnTo(dp.x,dp.z,true,dt);
        } else
      if(env.focus!=nullptr) {
        auto dp = env.focus->position()-pl.position();
        pl.turnTo(dp.x,dp.z,false,dt);
        }

      if(actrl[Act::ActLeft]) {
        moveFocus(env,Act::ActLeft);
        actrl[Act::ActLeft]  = false;
        }
      if(actrl[Act::ActRight]) {
        moveFocus(env,Act::ActRight);
        actrl[Act::ActRight]  = false;
        }
      if(!actrl[Act::ActForward]) {
        pl.setAnim(Npc::Anim::Idle);
        return;
        }
      }
    }

  if(actrl[Act::ActForward] || actrl[Act::ActMove]) {
    in.forwardKey        = actrl[Act::ActMove];
    actrl[Act::ActMove]  = false;
    if(ws!=WeaponState::Mage && !(env.g2Ctrl && (ws==WeaponState::Bow || ws==WeaponState::CBow))) {
      actrl[Act::ActForward] = false;
      if(!in.forwardKey)
        in.cancelMovement();
      }
    switch(ws) {
      case WeaponState::NoWeapon:
        break;
      case WeaponState::Fist:
        pl.fistShoot();
        return;
      case WeaponState::W1H:
      case WeaponState::W2H: {
        pl.swingSword();
        return;
        }
      case WeaponState::Bow:
      case WeaponState::CBow: {
        pl.shootBow(env.focus);
        return;
        }
      case WeaponState::Mage: {
        casting = (pl.beginCastSpell()==Npc::BC_Invest);
        if(!casting)
          actrl[Act::ActForward] = false;
        return;
        }
      }
    }

  if(actrl[Act::ActKill]) {
    if((ws==WeaponState::W1H || ws==WeaponState::W2H) && pl.target()!=nullptr && pl.canFinish(*pl.target()))
      pl.finishingMove();
    actrl[Act::ActKill] = false;
    }

  if(actrl[Act::ActLeft] || actrl[Act::ActRight] || actrl[Act::ActBack]) {
    auto ws = pl.weaponState();
    if(ws==WeaponState::Fist) {
      if(actrl[Act::ActBack])
        pl.blockFist();
      return;
      }
    else if(ws==WeaponState::W1H || ws==WeaponState::W2H) {
      if(actrl[Act::ActLeft] && pl.swingSwordL()) {
        in.cancelStrafe();
        }
      else if(actrl[Act::ActRight] && pl.swingSwordR()) {
        in.cancelStrafe();
        }
      else if(actrl[Act::ActBack] && pl.blockSword()) {
        // movement.forwardBackward.reset();
        }

      actrl[Act::ActLeft]  = false;
      actrl[Act::ActRight] = false;
      // actrl[ActBack]  = false;
      return;
      }
    else if(ws==WeaponState::Mage) {
      if(actrl[Act::ActLeft]) {
        moveFocus(env,Act::ActLeft);
        actrl[Act::ActLeft]  = false;
        }
      if(actrl[Act::ActRight]) {
        moveFocus(env,Act::ActRight);
        actrl[Act::ActRight]  = false;
        }
      }
    }

  if(in.wantsToStrafeLeft()) {
    ani = Npc::Anim::MoveL;
    }
  else if(in.wantsToStrafeRight()) {
    ani = Npc::Anim::MoveR;
    }
  else if(in.wantsToMoveForward()) {
    if((pl.walkMode()&WalkBit::WM_Dive)!=WalkBit::WM_Dive) {
      ani = Npc::Anim::Move;
      }
    else if(pl.isDive()) {
      pl.setDirectionY(rotY - rspeed);
      return;
      }
    }
  else if(in.wantsToMoveBackward()) {
    if((pl.walkMode()&WalkBit::WM_Dive)!=WalkBit::WM_Dive) {
      ani = Npc::Anim::MoveBack;
      } else if(pl.isDive()) {
      pl.setDirectionY(rotY + rspeed);
      return;
      }
    }


  if(in.jump) {
    if(pl.bodyStateMasked()==BS_JUMP) {
      ani = Npc::Anim::Idle;
      }
    else if(pl.isDive()) {
      ani = Npc::Anim::Move;
      }
    else if(pl.isSwim()) {
      pl.startDive();
      }
    else if(pl.isInWater()) {
      auto& g  = w.script().guildVal();
      auto  gl = pl.guild();

      if(0<=gl && gl<GIL_MAX && pl.isStanding()) {
        MoveAlgo::JumpStatus jump;
        jump.anim   = Npc::Anim::JumpUp;
        jump.height = float(g.jumpup_height[gl])+pl.position().y;
        pl.startClimb(jump);
        }
      }
    else if(pl.isStanding()) {
      auto jump = pl.tryJump();
      if(!pl.isFalling() && !pl.isSlide() && jump.anim!=Npc::Anim::Jump){
        pl.startClimb(jump);
        return;
        }
      ani = Npc::Anim::Jump;
      }
    else if(!pl.isAttackAnim() && !pl.isCasting()) {
      ani = Npc::Anim::Jump;
      }
    }

  if(!pl.isCasting()) {
    if(ani==Npc::Anim::Jump) {
      pl.setAnimRotate(0);
      rotation = 0;
      }

    if(pl.isAttackAnim()) {
      if((ani==Npc::Anim::MoveL || ani==Npc::Anim::MoveR/* || ani==Npc::Anim::MoveBack*/) && pl.hasState(BS_RUN)) {
        ani = Npc::Anim::Idle;
        }

      if(!pl.hasState(BS_RUN) && ani==Npc::Anim::Idle) {
        // charge-run
        ani = Npc::Anim::NoAnim;
        }
      if((ani==Npc::Anim::MoveL || ani==Npc::Anim::MoveR) &&
          pl.hasState(BS_STAND) && pl.hasState(BS_HIT)) {
        // no charge to strafe transition
        ani = Npc::Anim::NoAnim;
        }
      }

    if(bs==BS_LIE) {
      ani = (ani==Npc::Anim::Move) ? Npc::Anim::Idle : Npc::Anim::NoAnim;
      rot = pl.rotation();
      }

    if(ani!=Npc::Anim::NoAnim)
      pl.setAnim(ani);
    }

  setAnimRotate(pl, rot, ani==Npc::Anim::Idle ? rotation : 0, in.turn!=0.f, dt);
  if(actrl[Act::ActGeneric] || ani==Npc::Anim::MoveL || ani==Npc::Anim::MoveR || pl.isFinishingMove()) {
    processAutoRotate(pl,in,rot,dt);
    }

  assignRunAngle(pl,env,dt);
  pl.setDirection(rot);
  }

void PlayerMovement::implMoveMobsi(Npc& pl, PlayerIntent& in, uint64_t /*dt*/) {
  // animation handled in MOBSI
  auto inter = pl.interactive();

  if(in.backKey && !inter->isLadder()) {
    pl.setInteraction(nullptr);
    return;
    }

  if(inter->needToLockpick(pl) && !inter->isCracked()) {
    return;
    }

  if(!inter->isLadder() && inter->isStaticState() && !inter->isDetachState(pl)) {
    auto stateId = inter->stateId();
    if(inter->canQuitAtState(pl,stateId))
      pl.setInteraction(nullptr,false);
    }

  if(inter->isLadder()) {
    if(in.actionGeneric) {
      inter->onKeyInput(KeyCodec::ActionGeneric);
      in.actionGeneric = false;
      }
    else if(in.forwardKey) {
      inter->onKeyInput(KeyCodec::Forward);
      }
    else if(in.backKey) {
      inter->onKeyInput(KeyCodec::Back);
      }
    }
  }

void PlayerMovement::assignRunAngle(Npc& pl, const Env& env, uint64_t dt) {
  float dtF = (float(dt)/1000.f);

  float dest = 0;
  if(env.cameraAzimuth.has_value() && pl.walkMode()==WalkBit::WM_Run && pl.bodyState()==BS_RUN) {
    const float az   = *env.cameraAzimuth;
    const float maxV = 14.5f;
    dest = std::min(std::abs(az), maxV)*(az>=0 ? 1 : -1);
    }

  float a = std::min(dtF*5.f, 1.f);
  runAngleDest = runAngleDest*(1.f-a)+dest*a;
  }

void PlayerMovement::setAnimRotate(Npc& pl, float rotation, int anim, bool force, uint64_t dt) {
  float dtF    = (float(dt)/1000.f);
  float angle  = pl.rotation();
  float dangle = (rotation-angle)/dtF;
  auto& wrld   = pl.world();

  if(std::fabs(dangle)<30.f && !force) // 30 deg per second threshold
    anim = 0;
  if(anim!=0 && pl.isAttackAnim())
    anim = 0;
  if(rotationAni==anim && anim!=0)
    force = true;
  if(!force && wrld.tickCount()<turnAniSmooth)
    return;
  turnAniSmooth = wrld.tickCount() + 100;
  rotationAni   = anim;
  pl.setAnimRotate(anim);
  }

void PlayerMovement::processAutoRotate(Npc& pl, const PlayerIntent& in, float& rot, uint64_t dt) {
  if(auto other = pl.target()) {
    if(pl.weaponState()==WeaponState::NoWeapon || pl.isFinishingMove()){
      pl.setTarget(nullptr);
      }
    else if(!pl.isAttack()) {
      auto  dp   = other->centerPosition() - pl.centerPosition();
      auto  gl   = pl.guild();
      float step = float(pl.world().script().guildVal().turn_speed[gl]);
      if(in.combat[PlayerIntent::ActGeneric])
        step*=2.f;
      pl.rotateTo(dp.x,dp.z,step,AnimationSolver::TurnType::Std,dt);
      rot = pl.rotation();
      }
    }
  }
