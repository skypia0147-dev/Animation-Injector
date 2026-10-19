#pragma once
#include <cstdint>
#include <optional>
#include <string_view>

// Which camera view an action plays in (XML `view`, 0.12.225). Engine-free: parser, checker and tests.
//
//   <AnimationSet version="4" id="MyMod" view="first">        file default (absent: third)
//     <Action id="Dodge" view="third" firstPerson="switch">   per action (Action, Combo)
//
// A clip path is read under its project's folder: third person Meshes\Actors\Character\<clip>, first
// person Meshes\Actors\Character\_1stPerson\<clip>. `both` needs the file in both folders.
// Pairs and carries (and a carry's putdown Action) play in third person only: they ignore the file
// default, and another view written on them is a declaration error.
namespace AnimationInjector {
enum class View : std::uint8_t {
    kThird,  // third-person graphs only: NPCs, and the player in third person
    kFirst,  // the player's first-person graph only
    kBoth,   // both; the camera at the start picks the graph
};
// A third-person action the player starts in first person (XML firstPerson; only with view="third").
enum class FirstPersonStart : std::uint8_t {
    kBlock,   // does not start (as before 0.12.225)
    kSwitch,  // switches to third person, plays, and returns to first person when the player's run ends
};
struct ViewSpec {
    View view = View::kThird;
    FirstPersonStart firstPerson = FirstPersonStart::kBlock;
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
inline std::optional<FirstPersonStart> ParseFirstPersonStart(std::string_view text) {
    if (text == "block") return FirstPersonStart::kBlock;
    if (text == "switch") return FirstPersonStart::kSwitch;
    return std::nullopt;
}

// The player camera as a view (runtime/ViewGuard.h reads it): kOther is neither, such as a pair's
// animated camera, a kill cam, furniture, dialogue or a transition between the two.
enum class CameraView : std::uint8_t { kFirst, kThird, kOther };
constexpr bool Shows(View view, CameraView camera) {
    return camera == CameraView::kOther || (camera == CameraView::kFirst ? InFirst(view) : InThird(view));
}
// Whether the player may start an action of `spec` while the camera is in `camera`. A third-person action
// with firstPerson="switch" may start in first person (the camera then switches to third person).
constexpr bool MayStart(const ViewSpec& spec, CameraView camera) {
    return Shows(spec.view, camera) || (spec.view == View::kThird && spec.firstPerson == FirstPersonStart::kSwitch);
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
