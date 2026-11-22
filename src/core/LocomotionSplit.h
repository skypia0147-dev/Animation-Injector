#pragma once
#include "PCH.h"
#include "runtime/LadderSpec.h"
#include <functional>

// The direction blenders split into a walk clip and a run clip (<Locomotion>, docs/설계-이동사다리-완충칸.md section 7).
//
// Runs inside GraphInjector's CreateSymbolIdMap pass, after the actions, on the third-person character project's
// graphs, before the engine links them and clones them per actor. For every set the declaration covers
// (LocomotionSpec::Applies) it finds every hkbBlenderGenerator named `<set>_<Direction>Blend` in those graphs (the
// Block sets in BlockBehavior only, every other set everywhere but BlockBehavior: LadderInBlock), self-checks it, and
// adds two children after its vanilla slots:
//
//   on    vanilla slots  -1e6 + index   ...   walk clip  kWalkAxis    run clip  kRunAxis     axis = AI_Gait
//   off   vanilla slots  their weights  ...   walk clip  1e6          run clip  1e6 + 1      axis = SpeedSampled
//
// The walk clip copies the lowest vanilla slot that plays at 1x and the run clip the highest one (below the walk are
// the 0.1x Slow slots, above the run the 10x Fast ones, between them the strafe runs of the side ladders). On, the
// axis (LocomotionDrive, kMinAxis .. kRunAxis) lies between the two clips only, so nothing else of the blender plays;
// off (Toggle, [Debug] iLadderToggleKey) the blender is vanilla. No slot is added or dropped by Toggle and the array
// is ascending either way: a live blender's per-instance arrays are sized to its children.
//
// Both clips' playback speed is bound to a variable LocomotionDrive writes as the actor's Speed over the file's own
// ground speed, and the blender's blendParameter binding is pointed at LocomotionDrive's gait axis (in BlockBehavior
// at the axis that stays at the walk clip: moving while blocking is always the walk).
//
// The walk clip's child carries a bone mask (lower body 1, upper body kMaskedUpper, by bone name as vanilla's 2HW
// split): while the two mix, the legs blend and the upper body is the run's. Toggle sets the mask to all 1 and back;
// the one mask object is shared by the template children and their clones.
//
// A copy is a new hkbClipGenerator (NodeFactory, a node of the graph's budget) named after its source with a suffix
// and registered in animationdatasinglefile with the source's motion (AnimData::QueueCopies), so OAR, which replaces
// by animation file, still applies. The original clip nodes are never changed (other graphs share their names and
// data). A copy shares its source's clip triggers and gets its annotations, so the two clips align on the same sync
// point ids (FootLeft, FootRight): Havok matches sync points by id.
//
// Only this mod's blender: one whose slots differ from its vanilla record (core/VanillaLadders.h: slot count, clip
// names, weights, playback speeds) is rebuilt as vanilla first — another mod's added slots are dropped, its changed
// weights and speeds undone, a removed vanilla clip made again from the record — and logged with what differed.
// Another mod's edits inside a vanilla clip (its triggers, flags) stay.
//
// Self-check, per blender, read only (any failure: the blender is left as it was): a vanilla record exists for
// (graph, blender); flags has kParametricBlend; 1..32 children; empty per-instance arrays; the record has a separate
// walk and run slot that play different files. A blender of a declared name that is another ladder (aiming a bow)
// fails the last two and is left alone without a warning.
namespace AnimationInjector::LocomotionSplit {

// Whether `graph` may take `nodes` more nodes (GraphInjector's node id budget).
using RoomFn = std::function<bool(RE::hkbBehaviorGraph* graph, std::size_t nodes)>;

struct Summary {
    int blenders = 0;  // found with a declared name
    int patched = 0;
    int rebuilt = 0;  // another mod had changed them: rebuilt as vanilla (core/VanillaLadders.h) before patching
    int nodes = 0;    // clip generators created
};

// One project's graphs (the master and every graph the symbol map links). Reads Actions::Locomotion(); does nothing
// without a <Locomotion>. `character`: the project's character (the bone mask reads its animation skeleton's bone
// names).
Summary Apply(RE::hkbBehaviorGraph* master, const RE::BSScrapArray<RE::hkbBehaviorGraph*>& graphs,
    RE::hkbCharacter* character, const RoomFn& room);

// The bone mask is made once, from the animation skeleton of the character Apply ran with, and every character project
// shares it (the projects share the behavior graphs). Indexed by bone, so a project whose skeleton orders its bones
// differently would mask the wrong bones. Each third-person character project's symbol pass calls this: a skeleton
// not seen yet is compared bone by bone with the mask's classification, and one that differs is logged as a warning
// (vanilla's skeleton.hkx and skeleton_female.hkx are the same 99 bones in the same order, checked 11-20). Any thread.
void CheckMask(RE::hkbCharacter* character);

// In-game comparison with vanilla ([Debug] iLadderToggleKey). Main thread. Sets the child weights and the mask on the
// templates (actors loaded later) and on every loaded actor's graphs, and the axis (LocomotionDrive::SetEnabled).
// Starts on; not saved.
void Toggle();
bool Enabled();

}  // namespace AnimationInjector::LocomotionSplit
