#pragma once
namespace RE {
class hkbClipGenerator;
struct hkbContext;
}

// Was core/PairedPreloader.h until 0.12.224: every declared action animation is now prepared ahead of
// use, not only paired files.
namespace AnimationInjector::AnimationPreloader {
// Prepares declared animation files through real graph Update contexts, once per binding set (one
// character of the set drives the jobs, the player when it uses the set). The native Queue references
// are held for the process, as OAR holds its preloads: a graph teardown (every save load) hands the jobs
// to the set's next character instead of releasing them, so a later load does not read the files again.
// Only a timeout or a lost binding releases one (and prepares it again).
//
// What a binding set gets, paired files first:
//   - Paired files of its rig (the character, a pair-target creature): always. A pair cannot start on a
//     file that is not resident.
//   - Ordinary files (Action) in the projects of their action's view (runtime/ViewSpec.h): the
//     character's third-person ones, and the first-person one when the copy is there
//     (Actors\Character\_1stPerson\<file>, loose or in an archive): when [Preload] bOrdinary is on
//     (default), in declaration order. [Preload] iOrdinaryBudgetMB limits their
//     file size per project (default 0: no limit, as OAR); files past it load when played, as before
//     0.12.224. Sizes are read from the loose files once at Install, only when a budget is set; a file not
//     found on disk (an archive) counts as the average size.
// At most 16 files are queued per graph update; the engine reads them one after another. A file waits as long as
// the engine's queue moves (other requests, such as OAR's preloads, may come first); it times out and is queued
// again only when neither one of the set's files became ready nor the queue moved for 5 seconds.
//
// Does not activate a clip, issue an animation event, or retain an actor pointer.
void Install();
void ResetProgress();
// Defer all engine work to the next valid graph update.
void RequestCheck();
// A clip that failed to start on a file this set holds: prepare that file again.
void RecoverFailedMark(const RE::hkbContext& context, RE::hkbClipGenerator* clip);
void InstallCellEvents();
}
