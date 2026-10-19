#pragma once
#include "LinkHarness.h"
namespace AnimationInjector::PairRootRuntime {
inline bool busy = false;
inline bool Busy(PairedSession::Ticket) { return busy; }
}
