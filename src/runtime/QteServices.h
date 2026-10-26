#pragma once
#include "runtime/QteSpec.h"
#include "runtime/GameControls.h"
namespace RE {
class InputEvent;
}
namespace AnimationInjector::QteServices {
// A QTE on a game control (control="jump", 0.12.292; rightAttack since 0.12.23x): the keys the player has bound
// to it now, looked up as the QTE opens, so remapping is followed. The mouse button is the alternate key.
bool ResolveControl(QteStyle& style, GameControl control);
// The device the player last pressed (0.12.261): a key with both a keyboard/mouse key and a pad
// button shows only the one for this device. Before any press, the game's own gamepad setting decides.
void NoteInput(const RE::InputEvent* event);
bool UsingGamepad();
std::uint32_t ResolveSound(const QteResultSpec& result);
void PlaySound(std::uint32_t form);
// An event QTE result's action (0.12.249): started now, or when the engine hands a body getting up back.
// While the player plays a pair (0.12.264) both leave it: the player for `action`, the partner for `partnerAction`.
void StartAction(RE::ActorHandle actor, const std::string& action, const std::string& partnerAction = {});
}
