#pragma once
#include <cstdint>
#include <optional>
#include <string_view>

// Which camera view an action plays in (XML `view`, 0.12.225). Engine-free: parser, checker and tests.
//
//   <AnimationSet version="4" id="MyMod" view="first">        file default (absent: third)
//     <Action id="Dodge" view="third" otherView="switch">     per action (Action, Combo)
//
// A clip path is read under its project's folder: third person Meshes\Actors\Character\<clip>, first
// person Meshes\Actors\Character\_1stPerson\<clip>. `both` needs the file in both folders.
// Carries (and a carry's putdown Action) play in third person only: they ignore the file default, and another view
// written on them is a declaration error. A pair defaults to third whatever the file says and may be written first or
// both (1.0.8). both: the player plays the pair's first-person copy (Actors\Character\_1stPerson\<file>) in first
// person and the file itself in third person. first: the first-person copy is its one file, which every graph plays
// (the paired HKX holds both roles' tracks), and it starts only when the player is in first person.
namespace AnimationInjector {
enum class View : std::uint8_t {
    kThird,  // third-person graphs only: NPCs, and the player in third person
    kFirst,  // the player's first-person graph only
    kBoth,   // both; the camera at the start picks the graph
};
// An action of one view (third or first) the player starts in the other view (XML otherView, 1.0.8; not with
// view="both", nor on a first-person pair). Until 1.0.8 it was firstPerson, on third-person actions only.
enum class OtherViewStart : std::uint8_t {
    kBlock,   // does not start
    kSwitch,  // the camera switches to the action's view, plays, and returns when the player's run ends
};
struct ViewSpec {
    View view = View::kThird;
    OtherViewStart otherView = OtherViewStart::kBlock;
    bool operator==(const ViewSpec&) const = default;
};
constexpr bool InThird(View view) { return view != View::kFirst; }
constexpr bool InFirst(View view) { return view != View::kThird; }
inline std::optional<View> ParseView(std::string_view text) {
    if (text == "third") return View::kThird;
    if (text == "first") return View::kFirst;
    if (text == "both") return View::kBoth;
    return std::nullopt;
}
inline std::optional<OtherViewStart> ParseOtherViewStart(std::string_view text) {
    if (text == "block") return OtherViewStart::kBlock;
    if (text == "switch") return OtherViewStart::kSwitch;
    return std::nullopt;
}

// The player camera as a view (runtime/ViewGuard.h reads it): kOther is neither, such as a pair's
// animated camera, a kill cam, furniture, dialogue or a transition between the two.
enum class CameraView : std::uint8_t { kFirst, kThird, kOther };
constexpr bool Shows(View view, CameraView camera) {
    return camera == CameraView::kOther || (camera == CameraView::kFirst ? InFirst(view) : InThird(view));
}
// Whether the player may start an action of `spec` while the camera is in `camera`. An action of one view with
// otherView="switch" may start in the other view too (the camera then switches to its view).
constexpr bool MayStart(const ViewSpec& spec, CameraView camera) {
    return Shows(spec.view, camera) || (spec.view != View::kBoth && spec.otherView == OtherViewStart::kSwitch);
}
// The view the camera is moved to when a run of `run` (RunView: third or first) starts while the camera is in
// `camera`: the run's view when the camera is in the other one; kOther when there is nothing to switch.
constexpr CameraView SwitchFor(View run, CameraView camera) {
    if (run == View::kThird && camera == CameraView::kFirst) return CameraView::kThird;
    if (run == View::kFirst && camera == CameraView::kThird) return CameraView::kFirst;
    return CameraView::kOther;
}
// The fixed view of a run that starts with an action of `view` while the camera is in `camera`: `both`
// takes the camera's (third when the camera is neither).
constexpr View RunView(View view, CameraView camera) {
    if (view != View::kBoth) return view;
    return camera == CameraView::kFirst ? View::kFirst : View::kThird;
}
// The camera went from `before` to `after` (first or third person, not a switch the plugin made) during a
// run of `view`: the run is cancelled when `after` is a view it does not show. A scene camera never is.
constexpr bool CancelsRun(View view, CameraView before, CameraView after) {
    return after != before && after != CameraView::kOther && !Shows(view, after);
}
}
