#include "playercontrol.h"

#include <cmath>

#include "world/objects/npc.h"
#include "world/objects/item.h"
#include "world/objects/interactive.h"
#include "world/world.h"
#include "ui/dialogmenu.h"
#include "ui/inventorymenu.h"
#include "gothic.h"

PlayerControl::PlayerControl(DialogMenu& dlg, InventoryMenu &inv)
  :dlg(dlg),inv(inv) {
  Gothic::inst().onSettingsChanged.bind(this,&PlayerControl::setupSettings);
  setupSettings();
  }

PlayerControl::~PlayerControl() {
  Gothic::inst().onSettingsChanged.ubind(this,&PlayerControl::setupSettings);
  }

void PlayerControl::setupSettings() {
  if(Gothic::inst().version().game==2) {
    g2Ctrl = Gothic::inst().settingsGetI("GAME","USEGOTHIC1CONTROLS")==0;
    } else {
    g2Ctrl = false;
    }
  }

void PlayerControl::setTarget(Npc *other) {
  auto w  = Gothic::inst().world();
  auto pl = w ? w->player() : nullptr;
  if(pl==nullptr || pl->isFinishingMove())
    return;
  const auto ws    = pl->weaponState();
  const bool melle = (ws==WeaponState::Fist || ws==WeaponState::W1H || ws==WeaponState::W2H);
  if(other==nullptr) {
    if(!(melle && ctrl[Action::ActionGeneric])) {
      // dont lose focus in melee combat
      pl->setTarget(nullptr);
      }
    } else {
    pl->setTarget(other);
    }
  }

void PlayerControl::onKeyPressed(KeyCodec::Action a, Tempest::KeyEvent::KeyType key, KeyCodec::Mapping mapping) {
  auto       w    = Gothic::inst().world();
  auto       c    = Gothic::inst().camera();
  auto       pl   = w  ? w->player() : nullptr;
  auto       ws   = pl ? pl->weaponState() : WeaponState::NoWeapon;
  uint8_t    slot = pl ? pl->inventory().currentSpellSlot() : Item::NSLOT;

  if(w!=nullptr && w->isCutsceneLock())
    return;

  handleMovementAction(KeyCodec::ActionMapping{a,mapping}, true);

  if(pl!=nullptr && pl->interactive()!=nullptr && c!=nullptr && !c->isFree()) {
    auto inter = pl->interactive();
    if(inter->needToLockpick(*pl)) {
      processPickLock(*pl,*inter,a);
      return;
      }
    if(inter->isLadder()) {
      ctrl[a] = true;
      return;
      }
    }

  if(pl!=nullptr) {
    if(a==Action::Weapon) {
      if(ws!=WeaponState::NoWeapon) //Currently a weapon is active
        intent.weapon[PlayerIntent::WeaponClose] = true;
      else {
        if(intent.weaponLast>=PlayerIntent::Weapon3 && pl->inventory().currentSpell(static_cast<uint8_t>(intent.weaponLast-3))==nullptr)
          intent.weaponLast=PlayerIntent::WeaponBow;  //Spell no longer available -> fallback to Bow.
        if(intent.weaponLast==PlayerIntent::WeaponBow && pl->currentRangedWeapon()==nullptr)
          intent.weaponLast=PlayerIntent::WeaponMele; //Bow no longer available -> fallback to Mele.
        intent.weapon[intent.weaponLast] = true;
        }
      return;
      }

    if(a==Action::WeaponMele) {
      if(ws==WeaponState::Fist || ws==WeaponState::W1H || ws==WeaponState::W2H)
        intent.weapon[PlayerIntent::WeaponClose] = true; else
        intent.weapon[PlayerIntent::WeaponMele] = true;
      return;
      }

    if(a==Action::WeaponBow) {
      if(ws==WeaponState::Bow || ws==WeaponState::CBow)
        intent.weapon[PlayerIntent::WeaponClose] = true; else
        intent.weapon[PlayerIntent::WeaponBow] = true;
      return;
      }

    if(a>=Action::WeaponMage3 && a<=Action::WeaponMage10) {
      int id = (a-Action::WeaponMage3+3);
      if(ws==WeaponState::Mage && slot==id)
        intent.weapon[PlayerIntent::WeaponClose] = true; else
        intent.weapon[id] = true;
      return;
      }

    if(key==Tempest::KeyEvent::K_Return)
      ctrl[Action::K_ENTER] = true;
    }

  // this odd behaviour is from original game, seem more like a bug
  // const bool actTunneling = (pl!=nullptr && pl->isAttackAnim());
  const bool actTunneling = false;

  int fk = -1;
  if((ctrl[KeyCodec::ActionGeneric] || actTunneling) && !g2Ctrl) {
    if(a==Action::Forward) {
      if(pl!=nullptr && pl->target()!=nullptr && pl->canFinish(*pl->target()) && !pl->isAttackAnim()) {
        fk = PlayerIntent::ActKill;
        } else {
        fk = PlayerIntent::ActForward;
        }
      }
    if(ws==WeaponState::Fist || ws==WeaponState::W1H || ws==WeaponState::W2H) {
      if(a==Action::Back)
        fk = PlayerIntent::ActBack;
      }
    if(ws!=WeaponState::NoWeapon && !g2Ctrl && !pl->hasState(BS_RUN)) {
      if(a==Action::Left  || a==Action::RotateL)
        fk = PlayerIntent::ActLeft;
      if(a==Action::Right || a==Action::RotateR)
        fk = PlayerIntent::ActRight;
      }
    }

  if(g2Ctrl) {
    if(ws!=WeaponState::NoWeapon) {
      if(a==Action::ActionGeneric) {
        if(pl!=nullptr && pl->target()!=nullptr && pl->canFinish(*pl->target()) && !pl->isAttackAnim()) {
          fk = PlayerIntent::ActKill;
          } else {
          if(this->wantsToMoveForward())
            fk = PlayerIntent::ActMove; else
            fk = PlayerIntent::ActForward;
          }
        }
      }
    if(ws==WeaponState::Fist || ws==WeaponState::W1H || ws==WeaponState::W2H) {
      if(a==Action::Parade)
        fk = PlayerIntent::ActBack;
      }
    if(ws!=WeaponState::NoWeapon && !pl->hasState(BS_RUN)) {
      if(a==Action::ActionLeft)
        fk = PlayerIntent::ActLeft;
      if(a==Action::ActionRight)
        fk = PlayerIntent::ActRight;
      }
    }

  if(fk>=0) {
    intent.clearCombat();
    intent.combat[PlayerIntent::ActGeneric] = ctrl[KeyCodec::ActionGeneric];
    intent.combat[fk]         = true;

    ctrl[a] = true;
    return;
    }

  if(a==KeyCodec::ActionGeneric) {
    FocusAction fk = PlayerIntent::ActGeneric;
    if(this->wantsToMoveForward())
      fk = PlayerIntent::ActMove;
    intent.clearCombat();
    intent.combat[fk] = true;
    ctrl[a]   = true;
    return;
    }

  if(a==Action::Walk) {
    toggleWalkMode();
    return;
    }

  if(a==Action::Sneak) {
    toggleSneakMode();
    return;
    }

  if(a==Action::FirstPerson) {
    if(auto c = Gothic::inst().camera())
      c->setFirstPerson(!c->isFirstPerson());
    return;
    }

  if(a==Action::K_O && Gothic::inst().isMarvinEnabled())
    marvinO();

  ctrl[a] = true;
  }

void PlayerControl::onKeyReleased(KeyCodec::Action a, KeyCodec::Mapping mapping) {
  ctrl[a] = false;

  handleMovementAction(KeyCodec::ActionMapping{a, mapping}, false);

  auto w  = Gothic::inst().world();
  auto pl = w ? w->player() : nullptr;

  if(a==KeyCodec::Map && pl!=nullptr) {
    w->script().playerHotKeyScreenMap(*pl);
    }
  if(a==KeyCodec::Heal && pl!=nullptr) {
    w->script().playerHotLameHeal(*pl);
    }
  if(a==KeyCodec::Potion && pl!=nullptr) {
    w->script().playerHotLamePotion(*pl);
    }

  auto ws = pl==nullptr ? WeaponState::NoWeapon : pl->weaponState();
  if(ws==WeaponState::Bow || ws==WeaponState::CBow || ws==WeaponState::Mage) {
    if(a==KeyCodec::ActionGeneric || (!g2Ctrl && ws==WeaponState::Mage && a==KeyCodec::Forward))
      intent.clearCombat();
    } else {
    intent.clearCombat();
    }
  }

auto PlayerControl::handleMovementAction(KeyCodec::ActionMapping actionMapping, bool pressed) -> void {
  auto[action, mapping] = actionMapping;
  auto mappingIndex = (mapping == KeyCodec::Mapping::Primary ? size_t(0) : size_t(1));
  if (action == Action::Forward)
    movement.forwardBackward.main[mappingIndex] = pressed;
  else if (action == Action::Back)
    movement.forwardBackward.reverse[mappingIndex] = pressed;
  else if (action == Action::Right)
    movement.strafeRightLeft.main[mappingIndex] = pressed;
  else if (action == Action::Left)
    movement.strafeRightLeft.reverse[mappingIndex] = pressed;
  else if (action == Action::RotateR)
    movement.turnRightLeft.main[mappingIndex] = pressed;
  else if (action == Action::RotateL)
    movement.turnRightLeft.reverse[mappingIndex] = pressed;
  }

bool PlayerControl::isPressed(KeyCodec::Action a) const {
  return ctrl[a];
  }

void PlayerControl::onRotateMouse(float dAngleX, float dAngleY) {
  intent.rotMouse  += dAngleX;
  intent.rotMouseY += dAngleY;
  }

void PlayerControl::drawVobRay(DbgPainter& p) const {
  auto w = Gothic::inst().world();
  if(w==nullptr || w->player()==nullptr)
    return;
  auto pl    = w->player();
  auto focus = findFocus(&currentFocus);
  if(focus.interactive!=nullptr) {
    focus.interactive->drawVobRay(p, *pl);
    }
  if(focus.item!=nullptr) {
    focus.item->drawVobRay(p, *pl);
    }
  if(focus.npc!=nullptr) {
    pl->drawVobRay(p, *focus.npc);
    }
  }

void PlayerControl::tickFocus() {
  currentFocus = findFocus(&currentFocus);
  setTarget(currentFocus.npc);

  if(!ctrl[Action::ActionGeneric])
    return;

  auto focus = currentFocus;
  if(focus.interactive!=nullptr && interact(*focus.interactive)) {
    clearInput();
    }
  else if(focus.npc!=nullptr && interact(*focus.npc)) {
    clearInput();
    }
  else if(focus.item!=nullptr && interact(*focus.item)) {
    clearInput();
    }

  if(focus.npc)
    actionFocus(*focus.npc); else
    emptyFocus();
  }

void PlayerControl::clearFocus() {
  currentFocus = Focus();
  }

void PlayerControl::actionFocus(Npc& other) {
  setTarget(&other);
  }

void PlayerControl::emptyFocus() {
  setTarget(nullptr);
  }

Focus PlayerControl::focus() const {
  return currentFocus;
  }

bool PlayerControl::hasActionFocus() const {
  if(!ctrl[Action::ActionGeneric])
    return false;
  return currentFocus.npc!=nullptr;
  }

bool PlayerControl::interact(Interactive &it) {
  auto w = Gothic::inst().world();
  if(w==nullptr || w->player()==nullptr)
    return false;
  auto pl = w->player();
  if(w->player()->isDown())
    return true;
  if(!canInteract())
    return false;
  if(it.isContainer()){
    inv.open(*pl,it);
    return true;
    }
  if(pl->setInteraction(&it)){
    }
  return true;
  }

bool PlayerControl::interact(Npc &other) {
  auto w = Gothic::inst().world();
  if(w==nullptr || w->player()==nullptr)
    return false;
  auto pl = w->player();
  if(pl->isDown())
    return true;
  if(!canInteract())
    return false;
  auto state = pl->bodyStateMasked();
  if(other.isDown()) {
    if(state!=BS_STAND && state!=BS_SNEAK && state!=BS_SWIM && state!=BS_DIVE)
      return false;
    if(!inv.ransack(*w->player(),other))
      w->script().printNothingToGet();
    } else {
    if((state&BS_MAX)!=BS_NONE)
      return false;
    other.startDialog(*pl);
    }
  return true;
  }

bool PlayerControl::interact(Item &item) {
  auto w = Gothic::inst().world();
  if(w==nullptr || w->player()==nullptr)
    return false;
  auto pl = w->player();
  if(item.isTorchBurn() && pl->isUsingTorch())
    return false;
  if(pl->isDown())
    return true;
  if(!canInteract())
    return false;
  return pl->takeItem(item)!=nullptr;
  }

void PlayerControl::moveFocus(FocusAction act) {
  auto w = Gothic::inst().world();
  auto c = Gothic::inst().camera();
  if(w==nullptr || c==nullptr || currentFocus.npc==nullptr)
    return;

  auto vp  = c->viewProj();
  auto pos = currentFocus.npc->centerPosition();
  vp.project(pos);

  Npc* next = nullptr;
  auto npos = Tempest::Vec3();
  for(uint32_t i=0; i<w->npcCount(); ++i) {
    auto npc = w->npcById(i);
    if(npc->isPlayer())
      continue;
    auto p = npc->centerPosition();
    vp.project(p);

    if(std::abs(p.x)>1.f || std::abs(p.y)>1.f || p.z<0.f)
      continue;

    if(!w->testFocusNpc(npc))
      continue;

    if(act==PlayerIntent::ActLeft && p.x<pos.x && (next==nullptr || npos.x<p.x)) {
      npos = p;
      next = npc;
      }
    if(act==PlayerIntent::ActRight && p.x>pos.x && (next==nullptr || npos.x>p.x)) {
      npos = p;
      next = npc;
      }
    }

  if(next==nullptr)
    return;
  currentFocus.npc = next;
  }

void PlayerControl::toggleWalkMode() {
  auto w = Gothic::inst().world();
  if(w==nullptr || w->player()==nullptr)
    return;
  auto pl = w->player();
  pl->setWalkMode(pl->walkMode()^WalkBit::WM_Walk);
  }

void PlayerControl::toggleSneakMode() {
  auto w = Gothic::inst().world();
  if(w==nullptr || w->player()==nullptr)
    return;
  auto pl = w->player();
  if(!pl->canSneak() || (pl->walkMode()&WalkBit::WM_Sneak)==WalkBit::WM_Sneak) {
    pl->setWalkMode(pl->walkMode() & (~WalkBit::WM_Sneak));
    } else {
    pl->setWalkMode(pl->walkMode()^WalkBit::WM_Sneak);
    }
  }

bool PlayerControl::canInteract() const {
  auto w = Gothic::inst().world();
  if(w==nullptr || w->player()==nullptr)
    return false;
  auto pl = w->player();
  if(pl->weaponState()!=WeaponState::NoWeapon || pl->isAiBusy())
    return false;
  return true;
  }

void PlayerControl::clearInput() {
  movement.reset();
  std::memset(ctrl, 0,sizeof(ctrl));
  intent.clearCombat();
  intent.clearWeapon();
  }

void PlayerControl::marvinF8(uint64_t dt) {
  auto w = Gothic::inst().world();
  if(w==nullptr || w->player()==nullptr)
    return;

  auto& pl  = *w->player();
  auto  pos = pl.position();
  float rot = pl.rotationRad();
  float s   = std::sin(rot), c = std::cos(rot);

  Tempest::Vec3 dp(c,0.8f,s);
  pos += dp*6000*float(dt)/1000.f;

  pl.changeAttribute(ATR_HITPOINTS,pl.attribute(ATR_HITPOINTSMAX),false);
  pl.changeAttribute(ATR_MANA,     pl.attribute(ATR_MANAMAX),     false);
  pl.clearState(false);
  pl.clearSpeed();
  pl.clearAiQueue();
  pl.setPosition(pos);
  pl.setInteraction(nullptr,true);
  pl.setAnim(AnimationSolver::Idle);

  if(auto c = Gothic::inst().camera())
    c->reset();
  }

void PlayerControl::marvinK(uint64_t dt) {
  auto w = Gothic::inst().world();
  if (w == nullptr || w->player() == nullptr)
    return;

  auto& pl = *w->player();
  auto  pos = pl.position();
  float rot = pl.rotationRad();
  float s = std::sin(rot), c = std::cos(rot);

  Tempest::Vec3 dp(c, 0.0f, s);
  pos += dp * 6000 * float(dt) / 1000.f;

  pl.clearState(false);
  pl.clearSpeed();
  pl.setPosition(pos);
  pl.setInteraction(nullptr,true);
  // pl.setAnim(AnimationSolver::Idle); // Original G2 behaviour: K doesn't stop running
  }

void PlayerControl::marvinO() {
  auto w = Gothic::inst().world();
  if (w == nullptr || w->player() == nullptr || w->player()->target() == nullptr)
    return;

  auto target = w->player()->target();

  w->setPlayer(target);
  }

Focus PlayerControl::findFocus(const Focus* prev) const {
  auto w = Gothic::inst().world();
  auto c = Gothic::inst().camera();
  if(w==nullptr)
    return Focus();
  if(w->player()!=nullptr && w->player()->isDown())
    return Focus();
  if(c!=nullptr && c->isCutscene())
    return Focus();
  if(!cacheFocus)
    prev = nullptr;

  if(prev)
    return w->findFocus(*prev);
  return w->findFocus(Focus());
  }

bool PlayerControl::tickCameraMove(uint64_t dt) {
  auto w = Gothic::inst().world();
  if(w==nullptr)
    return false;

  Npc*  pl     = w->player();
  auto  camera = Gothic::inst().camera();
  if(camera==nullptr || (pl!=nullptr && !camera->isFree()))
    return false;

  intent.rotMouse = 0;
  if(ctrl[KeyCodec::Left] || (ctrl[KeyCodec::RotateL] && ctrl[KeyCodec::Jump])) {
    camera->moveLeft(dt);
    return true;
    }
  if(ctrl[KeyCodec::Right] || (ctrl[KeyCodec::RotateR] && ctrl[KeyCodec::Jump])) {
    camera->moveRight(dt);
    return true;
    }

  auto turningVal = movement.turnRightLeft.value();
  if(turningVal > 0.f)
    camera->rotateRight(dt);
  else if(turningVal < 0.f)
    camera->rotateLeft(dt);

  auto forwardVal = movement.forwardBackward.value();
  if(forwardVal > 0.f)
    camera->moveForward(dt);
  else if(forwardVal < 0.f)
    camera->moveBack(dt);
  return true;
  }

bool PlayerControl::tickMove(uint64_t dt) {
  auto w = Gothic::inst().world();
  if(w==nullptr)
    return false;
  Npc*  pl     = w->player();
  auto  camera = Gothic::inst().camera();

  if(w->isCutsceneLock())
    clearInput();

  if(tickCameraMove(dt))
    return true;

  if(ctrl[Action::K_F8] && Gothic::inst().isMarvinEnabled())
    marvinF8(dt);
  if(ctrl[Action::K_K] && Gothic::inst().isMarvinEnabled())
    marvinK(dt);
  cacheFocus = ctrl[Action::ActionGeneric];
  if(camera!=nullptr)
    camera->setLookBack(ctrl[Action::LookBack]);

  if(pl==nullptr)
    return true;

  updateIntent();

  PlayerMovement::Env env;
  env.g2Ctrl         = g2Ctrl;
  env.dialogActive   = dlg.isActive();
  env.printSpellName = true;
  env.focus          = currentFocus.interactive;
  if(camera!=nullptr)
    env.cameraAzimuth = camera->azimuth();
  env.moveFocus      = [this](FocusAction act){ moveFocus(act); };
  mvPlayer.tick(*pl,intent,env,dt);

  applyIntentFeedback();
  intent.rotMouseY = 0;
  return true;
  }

void PlayerControl::updateIntent() {
  intent.forward       = movement.forwardBackward.value();
  intent.strafe        = movement.strafeRightLeft.value();
  intent.turn          = movement.turnRightLeft.value();
  intent.actionGeneric = ctrl[Action::ActionGeneric];
  intent.forwardKey    = ctrl[Action::Forward];
  intent.backKey       = ctrl[Action::Back];
  intent.jump          = ctrl[Action::Jump];
  intent.transformBack = ctrl[Action::K_ENTER];
  intent.stopMove      = false;
  intent.stopStrafe    = false;
  }

void PlayerControl::applyIntentFeedback() {
  // movement code may consume keys: release them here, so they have to be pressed again
  ctrl[Action::ActionGeneric] = intent.actionGeneric;
  ctrl[Action::Forward]       = intent.forwardKey;
  ctrl[Action::K_ENTER]       = intent.transformBack;
  if(intent.stopMove)
    movement.reset();
  if(intent.stopStrafe)
    movement.strafeRightLeft.reset();
  }

void PlayerControl::processPickLock(Npc& pl, Interactive& inter, KeyCodec::Action k) {
  auto                   w             = Gothic::inst().world();
  auto&                  script        = w->script();
  const size_t           ItKE_lockpick = script.lockPickId();

  char ch = '\0';
  if(k==KeyCodec::Left || k==KeyCodec::RotateL)
    ch = 'L';
  else if(k==KeyCodec::Right || k==KeyCodec::RotateR)
    ch = 'R';
  else if(k==KeyCodec::Back) {
    quitPicklock(pl);
    return;
    }
  else
    return;

  auto cmp = inter.pickLockCode();
  while(pickLockProgress<cmp.size()) {
    auto c = cmp[pickLockProgress];
    if(c=='l' || c=='L' || c=='r' || c=='R')
      break;
    ++pickLockProgress;
    }

  if(pickLockProgress<cmp.size() && std::toupper(cmp[pickLockProgress])!=ch) {
    pickLockProgress = 0;
    const int32_t dex = Gothic::inst().version().game==2 ? pl.attribute(ATR_DEXTERITY) : (100 - pl.talentValue(TALENT_PICKLOCK));
    if(dex<=int32_t(script.rand(100)))  {
      script.invokePickLock(pl,0,1);
      pl.delItem(ItKE_lockpick,1);
      if(pl.inventory().itemCount(ItKE_lockpick)==0) {
        quitPicklock(pl);
        return;
        }
      } else {
      script.invokePickLock(pl,0,0);
      }
    } else {
    pickLockProgress++;
    if(pickLockProgress>=cmp.size()) {
      script.invokePickLock(pl,1,1);
      inter.setAsCracked(true);
      pickLockProgress = 0;
      } else {
      script.invokePickLock(pl,1,0);
      }
    }
  }

void PlayerControl::processLadder(Npc& pl, Interactive& inter, KeyCodec::Action key) {
  if(key!=KeyCodec::ActionGeneric && key!=KeyCodec::Forward && key!=KeyCodec::Back)
    return;

  ctrl[key] = true;
  inter.onKeyInput(key);
  }

void PlayerControl::quitPicklock(Npc& pl) {
  inv.close();
  pickLockProgress = 0;
  pl.setInteraction(nullptr);
  }

