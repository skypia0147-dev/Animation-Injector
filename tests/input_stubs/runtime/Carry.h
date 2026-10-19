#pragma once
#include "PCH.h"
namespace AnimationInjector::Carry {
// The ordinary input sink no longer reads carry input: the input dispatch hook claims an active
// carry's putdown key (Carry::ClaimInput) before PlayerControls.
}
