#include "Settings.h"
#include "declarations/XmlV2.h"
#include "runtime/ViewSpec.h"
#include <iostream>
#include <stdexcept>

// Camera views (runtime/ViewSpec.h, 0.12.225): the engine-free rules ViewGuard applies, and the
// view / firstPerson grammar.
using namespace AnimationInjector;
namespace {
unsigned checks = 0;
void Check(bool b, const char* text) {
    ++checks;
    if (!b) throw std::runtime_error(text);
}
struct Parsed {
    bool ok = false;
    std::vector<ActionSpec> actions;
    std::string error;
};
Parsed Parse(const std::string& body, const std::string& rootView = "") {
    const std::string xml = "<AnimationSet version='4' id='Test'" +
                            (rootView.empty() ? std::string() : " view='" + rootView + "'") + ">" + body +
                            "</AnimationSet>";
    pugi::xml_document doc;
    doc.load_string(xml.c_str());
    Parsed p;
    std::vector<EventRuleSpec> events;
    std::string mod;
    p.ok = XmlV2::Compile(doc, "test.xml", p.actions, events, mod, p.error, Declarations::ResolveKeyName);
    return p;
}
const ActionSpec* Find(const Parsed& p, const char* id) {
    for (const auto& a : p.actions)
        if (a.id == id) return &a;
    return nullptr;
}
}

int main() {
    try {
        using C = CameraView;
        // Start: an action starts where the camera shows its view; a third-person action with
        // firstPerson="switch" also in first person. A scene camera never blocks.
        const ViewSpec third{}, first{View::kFirst}, both{View::kBoth},
            thirdSwitch{View::kThird, FirstPersonStart::kSwitch};
        Check(MayStart(third, C::kThird) && !MayStart(third, C::kFirst), "third plays in third person only");
        Check(MayStart(first, C::kFirst) && !MayStart(first, C::kThird), "first plays in first person only");
        Check(MayStart(both, C::kFirst) && MayStart(both, C::kThird), "both plays in either");
        Check(MayStart(thirdSwitch, C::kFirst), "switch starts a third-person action from first person");
        Check(MayStart(third, C::kOther) && MayStart(first, C::kOther), "a scene camera does not block");

        // The run's view: `both` takes the camera at the start (third when it is a scene camera).
        Check(RunView(View::kBoth, C::kFirst) == View::kFirst && RunView(View::kBoth, C::kThird) == View::kThird &&
                  RunView(View::kBoth, C::kOther) == View::kThird && RunView(View::kFirst, C::kThird) == View::kFirst,
            "run views");

        // During: entering a view the run does not show cancels it; a scene camera or no change does not.
        Check(CancelsRun(View::kThird, C::kThird, C::kFirst), "third run, the camera goes first person: cancel");
        Check(CancelsRun(View::kFirst, C::kFirst, C::kThird), "first run, the camera goes third person: cancel");
        Check(!CancelsRun(View::kThird, C::kThird, C::kOther) && !CancelsRun(View::kThird, C::kOther, C::kThird),
            "a pair's animated camera and its recovery to third person are no change");
        Check(!CancelsRun(View::kThird, C::kFirst, C::kFirst), "the camera a run starts from is the baseline");
        Check(CancelsRun(View::kThird, C::kOther, C::kFirst), "first person after a scene camera: cancel");

        // Grammar: the file default, per action, and the third-only kinds.
        const std::string action = "<Action id='A' clip='a.hkx'/>";
        {
            const auto p = Parse(action + "<Action id='B' clip='b.hkx' view='both'/>"
                                          "<Action id='C' clip='c.hkx' view='third' firstPerson='switch'/>");
            Check(p.ok, "views parse");
            Check(Find(p, "A")->view == ViewSpec{}, "no view: third, block");
            Check(Find(p, "B")->view.view == View::kBoth, "both");
            Check(Find(p, "C")->view == thirdSwitch, "switch");
        }
        {
            const auto p = Parse(action + "<Pair id='P' clip='p.hkx'/>", "first");
            Check(p.ok, "a file default");
            Check(Find(p, "A")->view.view == View::kFirst, "the file's view");
            Check(Find(p, "P")->view.view == View::kThird, "a pair plays in third person whatever the file says");
        }
        Check(!Parse("<Pair id='P' clip='p.hkx' view='first'/>").ok, "a pair written in first person");
        Check(!Parse("<Action id='A' clip='a.hkx' view='first' firstPerson='switch'/>").ok,
            "firstPerson on a first-person action");
        Check(!Parse("<Action id='A' clip='a.hkx' view='side'/>").ok, "an unknown view");
        Check(!Parse(action, "side").ok, "an unknown file view");
        Check(!Parse("<Action id='A' clip='a.hkx' firstPerson='maybe'/>").ok, "an unknown firstPerson");
        std::cout << checks << " view checks passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
