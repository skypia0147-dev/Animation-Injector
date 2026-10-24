#include "runtime/ViewGuard.h"
#include "runtime/ActionLinks.h"
#include "runtime/ActionRegistry.h"
#include "runtime/ActionRunner.h"
#include "runtime/Carry.h"
#include "runtime/EventRegistry.h"
#include "runtime/PairFeatures.h"
#include "runtime/PairedSession.h"
#include "Settings.h"

namespace AnimationInjector::ViewGuard {
namespace {
struct Run {
    bool active = false;
    View view = View::kThird;
    bool switched = false;   // the plugin moved the camera from first to third person for it
    bool cancelled = false;  // a view change ended it; nothing is restored
    float idle = 0.0f;       // seconds with nothing playing: a link's gap is not an end
};
Run run;
CameraView last = CameraView::kOther;  // the last first- or third-person view seen
CameraView expected = CameraView::kOther;
float expectedAge = 0.0f;
bool restorePending = false;          // back to first person once the pair camera is released
constexpr float kEndGrace = 0.3f;     // between two linked actions nothing may run for a few frames
constexpr float kExpectLimit = 3.0f;  // a switch the plugin makes settles well within this

// The player's own third-person zoom (0.12.288): the last one seen in third person outside a run the plugin switched,
// given back when a first-person start switches to third person (the engine's own is its closest). None seen yet:
// the INI's [Camera] fSwitchZoom.
float rememberedZoom = 0.0f;
bool zoomRemembered = false;
bool zoomCheck = false;  // log the zoom the frame after a switch: whether the engine kept it
RE::ThirdPersonState* ThirdPerson() {
    auto* camera = RE::PlayerCamera::GetSingleton();
    auto* state = camera ? camera->currentState.get() : nullptr;
    return state && state->id == RE::CameraStates::kThirdPerson ? static_cast<RE::ThirdPersonState*>(state) : nullptr;
}
void RememberZoom() {
    if (run.switched || restorePending) return;
    if (auto* state = ThirdPerson(); state && std::isfinite(state->targetZoomOffset)) {
        rememberedZoom = state->targetZoomOffset;
        zoomRemembered = true;
    }
}
void ApplyZoom() {
    auto* state = ThirdPerson();
    if (!state) {
        logger::info("[ViewGuard] third-person zoom not set: the camera is not in third person after the switch");
        return;
    }
    const float zoom = zoomRemembered ? rememberedZoom : FrameworkSettings::Get().switchZoom;
    const float target = state->targetZoomOffset, current = state->currentZoomOffset, saved = state->savedZoomOffset;
    state->targetZoomOffset = state->currentZoomOffset = state->savedZoomOffset = zoom;
    zoomCheck = true;
    logger::info("[ViewGuard] third-person zoom {:.3f} ({}): engine target={:.3f} current={:.3f} saved={:.3f}", zoom,
        zoomRemembered ? "last used" : "INI fSwitchZoom", target, current, saved);
}
const char* Name(CameraView camera) {
    return camera == CameraView::kFirst ? "first" : camera == CameraView::kThird ? "third" : "scene";
}
const char* Name(View view) { return view == View::kFirst ? "first" : view == View::kBoth ? "both" : "third"; }
void Switch(CameraView to) {
    auto* camera = RE::PlayerCamera::GetSingleton();
    if (!camera) return;
    ExpectCamera(to);
    if (to == CameraView::kThird) {
        camera->ForceThirdPerson();
        ApplyZoom();
    } else
        camera->ForceFirstPerson();
}
// What the player plays now and the view it plays in (third for a pair or a carry).
bool Playing(RE::PlayerCharacter* player, View& view) {
    const auto handle = player->GetHandle();
    PairedSession::Ticket ticket;
    if (PairedSession::TryGetTicket(handle, ticket) || Carry::BlocksAction(player)) {
        view = View::kThird;
        return true;
    }
    int action = -1;
    Actions::FindRunner(player, [&](int index, ActionRunner& runner) {
        if (runner.GetState() == ActionRunner::State::kIdle) return false;
        action = index;
        return true;
    });
    if (action >= 0) {
        view = Actions::Spec(action).view.view;
        return true;
    }
    return ActionLinks::HasPendingTransition(handle);  // keeps the running view
}
void Cancel(RE::PlayerCharacter* player, CameraView camera) {
    logger::info("[ViewGuard] camera {} during a {} run: cancelling it", Name(camera), Name(run.view));
    run.cancelled = true;
    run.switched = false;
    restorePending = false;
    const auto handle = player->GetHandle();
    PairedSession::Ticket ticket;
    if (PairedSession::TryGetTicket(handle, ticket)) PairedSession::Cancel(ticket);
    Carry::Cancel("view-changed");
    Actions::CancelPlayerRun("view-changed");
    EventRegistry::DropDelayed(handle);
}
// Our actions set bAnimationDriven through the setter that writes every graph of the actor. A state that
// takes the control over (a pair, a cancel-out attack) turns it off only in the graph it plays in, so the
// other view's graph keeps it; in first person the view then only looks up and down (0.12.226 probe:
// driven=true, look input, no heading change). After a run, with nothing of ours left, it is turned off in
// every graph once: in first person at once (our third-person actions never play there), in third person
// once the playing graph has let it go (a cancel-out attack may still use it).
bool drivenSync = false;
void SyncDriven(RE::PlayerCharacter* player, CameraView camera) {
    static const RE::BSFixedString driven("bAnimationDriven");
    if (camera == CameraView::kOther) return;
    bool on = false;
    if (camera == CameraView::kThird && (!player->GetGraphVariableBool(driven, on) || on)) return;
    drivenSync = false;
    player->SetGraphVariableBool(driven, false);
    logger::debug("[ViewGuard] run over: bAnimationDriven off in every graph (camera {})", Name(camera));
}
}

CameraView Current() {
    auto* camera = RE::PlayerCamera::GetSingleton();
    if (!camera || !camera->currentState) return CameraView::kOther;
    switch (camera->currentState->id) {
    case RE::CameraStates::kFirstPerson: return CameraView::kFirst;
    case RE::CameraStates::kThirdPerson: return CameraView::kThird;
    default: return CameraView::kOther;
    }
}
bool AllowStart(int action) {
    if (action < 0 || action >= Actions::Count()) return true;
    // A selecting action plays nothing: the action its Flow picks is checked when it starts (0.12.287). Its own
    // view (third, first person blocked by default) refused every first-person start of a mod that picks its
    // dodge by direction (Dynamic Dodge Shot), though each dodge it picks switches to third person.
    if (Actions::Spec(action).SelectsOnly()) return true;
    return MayStart(Actions::Spec(action).view, Current());
}
void ExpectCamera(CameraView to) {
    expected = to;
    expectedAge = 0.0f;
}
void Update(float delta) {
    auto* player = RE::PlayerCharacter::GetSingleton();
    if (!player || !player->Is3DLoaded()) return;
    const auto camera = Current();
    if (zoomCheck) {
        zoomCheck = false;
        if (auto* state = ThirdPerson())
            logger::info("[ViewGuard] third-person zoom a frame later: target={:.3f} current={:.3f}",
                state->targetZoomOffset, state->currentZoomOffset);
    }
    RememberZoom();
    const bool ours = expected != CameraView::kOther && camera == expected;
    if (expected != CameraView::kOther && (ours || (expectedAge += delta) > kExpectLimit))
        expected = CameraView::kOther;

    View view = View::kThird;
    if (Playing(player, view)) {
        run.idle = 0.0f;
        if (!run.active) {
            // The camera it starts from is the baseline; a later change is measured against it.
            if (camera != CameraView::kOther) last = camera;
            run = {};
            run.active = true;
            run.view = RunView(view, last);
            // A run right after one we switched plays on in that third-person view: restore after this one.
            run.switched = restorePending;
            restorePending = false;
            if (run.view == View::kThird && camera == CameraView::kFirst) {
                Switch(CameraView::kThird);
                run.switched = true;
            }
            logger::debug(
                "[ViewGuard] run begins view={} camera={} switched={}", Name(run.view), Name(camera), run.switched);
        }
    } else if (run.active && (run.idle += delta) >= kEndGrace) {
        if (run.switched && !run.cancelled) restorePending = true;
        drivenSync = true;
        logger::debug(
            "[ViewGuard] run ends view={} cancelled={} restore={}", Name(run.view), run.cancelled, restorePending);
        run = {};
    }

    if (camera != CameraView::kOther) {
        if (run.active && !run.cancelled && !ours && CancelsRun(run.view, last, camera)) Cancel(player, camera);
        last = camera;
    }

    if (restorePending && !run.active) {
        if (camera == CameraView::kFirst)
            restorePending = false;  // already there (the player went back themselves)
        else if (camera == CameraView::kThird && !PairFeatures::CameraBusy()) {
            restorePending = false;
            logger::info("[ViewGuard] run ended: back to first person");
            Switch(CameraView::kFirst);
        }
    }
    if (drivenSync && !run.active) SyncDriven(player, camera);
}
void OnGameLoad() {
    run = {};
    last = CameraView::kOther;
    expected = CameraView::kOther;
    expectedAge = 0.0f;
    restorePending = false;
    drivenSync = false;  // the graphs are new
}
}
