#pragma once
#include "PCH.h"
#include "runtime/GaitShare.h"

// The per-actor values the split direction blenders read (<Locomotion>, docs/설계-이동사다리-완충칸.md section 7).
//
//   kGaitVariable  the blend axis: kWalkAxis the walk clip, kRunAxis the run clip (core/LocomotionSplit.h).
//   AI_Rate*       the clips' playback speed: the actor's Speed over the file's own ground speed, so the feet keep
//                  the ground whatever moves the actor faster or slower (a plugin's movement types, SpeedMult, the
//                  weight carried). Measured 11-16: vanilla's own 10x slot speeds a run up 7% at 1.6x the speed.
//   kDirectionVariable  Direction while moving, held while the speed is 0 (a diagonal stop ends with the engine
//                  turning Direction to the straight side in one frame, 11-15).
//
// The gait (the run's share, 0 walk .. 1 run), from the actor's walking / running state and its Speed against
// Actor::GetWalkSpeed(), which is the steady walking Speed whatever changed it (measured 11-16 at SpeedMult 70..160 and
// with the movement types edited):
//   walking  the share is the Speed's way down from 2.2x to 1.08x the walk speed, eased with kWalkEase. The state
//            flips with the key while the Speed takes 0.2 .. 0.3 s to follow; following the state alone showed the
//            walk at 3 .. 5x its rate (11-16).
//   running  up: eased with kRunEase. Down, below 1.8x the walk speed (a stop): linear, kGaitSeconds for the whole
//            way, and no lower than kStopRunShare — with the walk clip's bone mask (LocomotionSplit) the legs end the
//            stop walking and the upper body keeps the run. Standing, the share stays (the running state clears at
//            once, and following it turned the upper body from run to walk to idle within the stop's cross-fade).
// These are the user's choice among three in-game variants (11-16, "C").
//
// Once a frame for every actor, before the engine updates it (Character::Update for NPCs, PlayerCharacter's
// UpdateAnimation for the player; see Install), in each of its graphs that has kGaitVariable, the inactive ones too:
// the player's third-person graph keeps stepping in first person (11-20).
// Nothing is read until a split blender was bound; after that an actor whose graphs have none costs one failed lookup
// per graph. The rate variables are written only on a frame the actor's Speed moved more than 0.5% (at least 0.5) from
// the one last written (kStampVariable): measured 0.5 us an update when they are not.
// Off (LocomotionSplit::Toggle): the axis takes SpeedSampled, as vanilla.
namespace AnimationInjector::LocomotionDrive {

inline constexpr const char* kGaitVariable = "AI_Gait";
inline constexpr const char* kDirectionVariable = "AI_LadderDirection";
// The direction before a slow reversal (LocomotionDrive's opposite turns), one per graph with kDirectionVariable.
inline constexpr const char* kTurnAnchorVariable = "AI_LadderTurnAnchor";
// Seconds the direction has been held at the start of a slow sweep (LocomotionDrive's opposite turns).
inline constexpr const char* kTurnHoldVariable = "AI_LadderTurnHold";
// Turn events (11-21 left and right, 11-22 forward and back; user decisions: running only, one clip each): the moment
// the player presses the opposite way while running one way without facing it (sideways, or backward and forward as
// the keys move the body: not TDM's directional setting, which turns the body around), the runtime sends one of these
// to the player's graph; a declaration starts its pivot action on it (<Start event="AI_Turn_RightToLeft" to="...">).
// Sent at the key press, before the engine slows down (about 0.25 s), so the clip plays the braking too. With no
// declaration they reach nothing.
inline constexpr const char* kTurnRightToLeftEvent = "AI_Turn_RightToLeft";
inline constexpr const char* kTurnLeftToRightEvent = "AI_Turn_LeftToRight";
inline constexpr const char* kTurnForwardToBackEvent = "AI_Turn_ForwardToBack";
inline constexpr const char* kTurnBackToForwardEvent = "AI_Turn_BackToForward";

// The notify hook, for CyclicFreeze and CyclicCrossBlend sent to `actor`: one the engine sent itself starts its own
// cross blend, which the opposite turns then leave alone (they ignore their own sends).
void NoteCyclicEvent(RE::Actor* actor);
// Load and new game: the opposite turns' pending cross blends, the engine's recent ones and the turn events' state go
// with the graphs they were for.
void ResetOnLoad();
inline constexpr const char* kStampVariable = "AI_RateStamp";
// The axis of the blenders that only ever walk (moving while blocking): kMinAxis, or SpeedSampled when off.
inline constexpr const char* kWalkOnlyVariable = "AI_GaitWalk";
inline constexpr float kWalkAxis = 1.0f, kRunAxis = 2.0f;
// The lowest axis: just above the walk clip, so the blend is always between the walk and the run clip and never
// between the walk clip and the vanilla slot parked below it (a parametric blend keeps both ends of its interval
// active, and an active clip sends its triggers).
inline constexpr float kMinAxis = 1.001f;
inline constexpr float kMinRate = 0.1f, kMaxRate = 10.0f;
// The gait itself: runtime/GaitShare.h (Gait::NextShare and its constants).

// Graph pass. Points the blender's blendParameter binding (a SpeedSampled binding) at kGaitVariable, adding the
// variable to `graph` once. False (the blender keeps SpeedSampled; one warning) when the binding is not as expected.
bool Rebind(RE::hkbBehaviorGraph* graph, RE::hkbNode* blender);
// The same, at kWalkOnlyVariable.
bool RebindWalk(RE::hkbBehaviorGraph* graph, RE::hkbNode* blender);
// Graph pass. Points a BSCyclicBlendTransitionGenerator's fBlendParameter binding (a Direction binding; the direction
// blend under it has no binding of its own and takes the value from it) at kDirectionVariable. False, quietly, when it
// is not bound to Direction.
bool RebindDirection(RE::hkbBehaviorGraph* graph, RE::hkbNode* cyclic);

// Graph pass. A vanilla clip whose binding set binds playbackSpeed alone (vanilla binds its attack clips so); new sets
// are copies of it with the variable index changed, which keeps the engine's own member path data.
bool IsRateBinding(const RE::hkbNode* clip);
void SetRateSample(RE::hkbVariableBindingSet* sample);
// Graph pass. The clip's playbackSpeed becomes a variable the hook writes as Speed / fileSpeed. Clips of one file
// speed share a variable. `ownFile` (an own tree's dedicated file, its animation path): a variable of its own, whose
// file speed FileSpeedKnown replaces once the file's own is read. False: no sample, or the graph data is not as
// expected; the clip keeps its fixed speed.
bool BindRate(RE::hkbBehaviorGraph* graph, RE::hkbNode* clip, float fileSpeed, std::string_view ownFile = {});
// Clip activation (core/ClipHooks): whether `animationPath` is a dedicated file whose own speed is not known yet.
// One atomic read when none is waiting.
bool WantsFileSpeed(const char* animationPath);
// The dedicated file's own ground speed: the last "animmotion x y z" annotation's distance over the file's length.
void FileSpeedKnown(const char* animationPath, float speed);

// The switches at the places where an own tree replaced vanilla's node (core/OwnLocomotion.h Switch): an int
// variable, 0 while the split is on and 1 while LocomotionSplit::Toggle turned it off, written per actor each frame.
inline constexpr const char* kOffVariable = "AI_LocomotionOff";
// Graph pass. A vanilla hkbManualSelectorGenerator of `graph` whose selectedGeneratorIndex is bound to an int
// variable: the first one noted is the sample the switches are copied from (the node, and its binding).
void NoteSwitchSample(RE::hkbBehaviorGraph* graph, RE::hkbNode* selector);
const RE::hkbNode* SwitchSample();
// Graph pass. Binds a switch's selectedGeneratorIndex to kOffVariable, adding the variable to `graph` once: a new
// set with the sample's one binding pointed at it. False: no sample, or the graph data is not as expected.
bool BindSwitch(RE::hkbBehaviorGraph* graph, RE::hkbNode* selector);

// The per-actor update hook. Plugin load.
void Install();
// LocomotionSplit::Toggle: off = the axis follows SpeedSampled exactly (vanilla).
void SetEnabled(bool on);

}  // namespace AnimationInjector::LocomotionDrive
