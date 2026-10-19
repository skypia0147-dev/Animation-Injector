#pragma once
#include "PCH.h"

// Actor hooks (PlayerCharacter + Character): NotifyAnimationGraph blocks stagger events during
// HyperArmor/IFrame windows and routes native cancels and event triggers. IFrame damage immunity
// is ActorImmunity (the actor's own ghost state); HandleHealthDamage (0x104) was never called
// on the damage path and is no longer hooked.
namespace AnimationInjector::PlayerHooks {
void Install();
}
