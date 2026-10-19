#pragma once
#include "PCH.h"
namespace AnimationInjector::QteServices {
// The QTE key's device choice is not part of the input dispatcher's behaviour under test.
inline void NoteInput(const RE::InputEvent*) {}
}
