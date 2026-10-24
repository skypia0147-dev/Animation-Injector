#pragma once
#include "PCH.h"
#include "runtime/ViewSpec.h"

// The player's camera view against the views of what it is playing (0.12.225). The rules themselves are
// engine-free (runtime/ViewSpec.h: MayStart, RunView, CancelsRun).
//
// A run: the player's stretch of our playback, from its first action, carry or pair until none of them
// and no pending link has remained for a moment (the stretch of its EventEffects run scope). Its view is
// fixed at the start: third for a pair or a carry, else the action's view (for `both`, the camera's).
//   - Start: an action of the player whose view the camera does not show does not start, unless it is a
//     third-person action with firstPerson="switch". A run that starts in third person while the camera is
//     in first person (that switch, or a pair another actor starts on the player, as vanilla kill moves
//     do) switches the camera to third person.
//   - During: when the camera enters first or third person and the run does not show that view, the whole
//     run is cancelled through its own end paths: the player's actions and pending links (their exit
//     blend), its carry (the target is dropped, as when the carrier is hit) and its pair (both actors).
//     Holds end with the run scope, so a hidden weapon shows and slow time, invulnerability and locks end;
//     pending delayed event functions that the run's actors sent are dropped. A scene camera (CameraView
//     kOther) is no change, nor is a switch this plugin makes itself (ExpectCamera: a pair's camera
//     recovery, the switches above).
//   - End: a run the plugin switched from first to third person returns to first person when it ends
//     without a cancel, once the pair camera is released. A cancelled run leaves the camera where the
//     player put it.
// Controls are never disabled: the view key and the zoom stay the player's.
namespace AnimationInjector::ViewGuard {
// Main thread. The player camera's state as a view.
CameraView Current();
// Main thread, the player's runner checking whether `action` may start: false when its view does not fit
// the camera (MayStart). A selecting action always may: the action it picks is checked (0.12.287). No side effect:
// a switch happens when the run has started (Update).
bool AllowStart(int action);
// Main thread: this plugin is about to move the player's camera to `to`; not a change of the player's.
void ExpectCamera(CameraView to);
// Main thread, every player update after the runners, the carry and the pairs.
void Update(float delta);
// Load / new game: forgets the run. Nothing is restored: controls are never touched.
void OnGameLoad();
}
