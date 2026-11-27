#pragma once
#include <string>

// Optional actions (<Action optional="true">, 1.1.0): an add-on that brings an animation framework's behavior (the MCO
// add-on) declares every clip that framework can play, though a moveset ships only some (MCO's charge loops and
// transitions). An optional action whose clip is installed nowhere is not made: it gets no state in any graph, and a
// row, a transition or an on="end" that goes to it is passed over as if its condition were false.
//
// Installed means the clip's own path (loose or in an archive, the engine's lookup) or a file of the same name in an
// animation replacer's folders (OAR's OpenAnimationReplacer, DAR's DynamicAnimationReplacer; both read loose files
// only). A moveset that has the clip for one weapon but not another is installed: that actor plays it as the replacer
// gives it.
namespace AnimationInjector::ClipPresence {
// Whether `action` is an optional action whose clip is installed nowhere. Worked out once, on the first call after the
// game's data is loaded (any thread); false for every other action.
bool Absent(int action);
// Whether `file` (a declared clip path, any case) is the clip of such an action: its name is not registered in the
// projects either (an engine table with a fixed size).
bool AbsentFile(const std::string& file);
// Whether an optional action's clip, activating for an actor, has nothing to play: the animation replacer gave it no
// file (`replaced` false) and its own path has none. A moveset that has the clip for one weapon but not another: the
// other weapon's actor gets an empty clip, which then ends at once (ActionRunner), as MCO's graph passes a clip that
// has no file.
bool NothingToPlay(int action, bool replaced);
}
