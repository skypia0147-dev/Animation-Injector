#pragma once
#include "runtime/PairedSession.h"
namespace AnimationInjector::PairRootRuntime {
void Install();
void Reset();
void Sample(float delta, bool afterRuntime);
bool Busy(PairedSession::Ticket ticket);
void ObserveClip(PairedSession::Ticket ticket, RE::ActorHandle actor, const RE::hkbContext& context,
    const RE::BSSynchronizedClipGenerator& generator, bool exiting);
}
