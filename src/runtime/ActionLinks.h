#pragma once
#include "PCH.h"
#include "runtime/GameControls.h"
#include "runtime/HeldKeys.h"
#include "runtime/PressKinds.h"
namespace AnimationInjector::ActionLinks {
void Begin(RE::ActorHandle actor, int action, bool paired, bool player, RE::ActorHandle partner = {});
void End(RE::ActorHandle actor, int action);
void ObserveEvent(RE::ActorHandle actor, const RE::BSFixedString& event);
void ObserveTime(RE::ActorHandle actor, int action, float time, float duration = 0.0f);
// Claim Connect keys for the whole source lifetime, including key-up events. `control` is the game control the
// press stands for (0.12.292): a transition on a control matches it from any device. `taken` = an open window took
// this press, or the press waits to tell its kind (a press on a game control is then kept from the game).
//
// How a key is pressed (1.0.8, <Transition press="tap|double|hold">): each source keeps one press classifier per key
// (runtime/PressKinds.h, the one a start's key has). A press="down" transition takes the press the moment it goes
// down, as before. Otherwise the press waits: a double goes at the second press, a hold at the hold time, a tap once
// the key is up and no second press came. A press no transition takes is dropped: a game control's is not handed to
// the game late, as a start's is (the running action has the key).
// `kept` = the press is the source's without having gone anywhere yet (it waits to tell its kind, or a double just
// went on it): a game control's is kept from the game like a taken one, but it does not become the key an
// on="release" transition waits for until its transition goes.
struct InputResult {
    bool claimed = false, taken = false, kept = false;
};
enum class InputPhase : std::uint8_t { kDown, kHeld, kUp };  // a new press, a held repeat, the release
// `now`: the input sink's game clock (the one HeldKeys is fed with); below 0 keeps the clock Update adds up.
InputResult Input(std::uint32_t code, bool gamepad, GameControl control = GameControl::kNone,
    InputPhase phase = InputPhase::kDown, float now = -1.0f);
// The hold time and double window the classifiers use ([Input] fPressHoldTime, fPressDoubleWindow), as the input
// sink's.
void SetPressTiming(PressKinds::Timing timing);
// A press told apart while its source was already leaving (exit or recovery sent, the clip still blending out): no
// transition can go from it, so the input sink starts what its key starts when pressed that way. A press that
// goes down while the source is leaving is also given at once as kDown, for the starts that go the moment their
// key goes down, and still told apart if none takes it. Taken once.
struct Forward {
    HeldKeys::Key key;
    PressKinds::Press press = PressKinds::Press::kDown;
    float downAt = 0;    // when its first press went down
    int direction = -1;  // the movement direction then (MoveDir), -1 unknown
};
std::vector<Forward> TakeForwards();
// A start took a press the moment it went down (a Forward of kDown): it is no longer told apart.
void DropPress(HeldKeys::Key key);
// An event a running action of `actor` has a transition on (10-29), as Input claims a key: its window open, not open
// yet or closed, or that transition's destination still waiting. An event start ignores it then: the transition
// takes the event (a closed window turns it into no new start either). Any thread.
bool ClaimsEvent(RE::ActorHandle actor, const RE::BSFixedString& event);
void Update(float delta);
// Charge (10-30). The runner of `action` reached its Exit point: an on="end" transition goes instead (true), else
// false and the runner exits as usual. Main thread.
bool ReachedEnd(RE::ActorHandle actor, int action);
// A loop's maxHold passed (an NPC's, or a player still holding): its key counts as released, so an on="release"
// transition goes (true); false when it has none, and the runner exits. Main thread.
bool ForceRelease(RE::ActorHandle actor, int action);
void ResetOnLoad();
// Main thread (ViewGuard): drops the actor's link windows and pending transitions, as a partner too.
void Cancel(RE::ActorHandle actor);
// An event QTE's result (0.12.264): the pair `actor` plays hands both over like a Connect with partnerTo, `actor` to
// `target` and its partner to `partnerTarget` (-1: none). False when `actor` plays no pair. Main thread.
bool RequestHandoff(RE::ActorHandle actor, int target, int partnerTarget);
// Keep inherited camera ownership while an actual destination is pending.
bool HasPendingTransition(RE::ActorHandle actor);
}
