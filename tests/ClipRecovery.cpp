#include "Settings.h"
#include "runtime/DeclarationCatalog.h"
#include <iostream>
#include <stdexcept>
using namespace AnimationInjector;
void Check(bool value, const char* name) {
    if (!value) throw std::runtime_error(name);
}
bool Near(float a, float b) { return std::fabs(a - b) < 0.0001f; }
const ActionSpec* Find(const std::vector<ActionSpec>& specs, const char* id) {
    for (const auto& s : specs)
        if (s.id == id) return &s;
    return nullptr;
}
int wmain(int argc, wchar_t** argv) {
    try {
        Check(argc == 2, "XML argument required");
        std::vector<ActionSpec> specs;
        Check(Declarations::LoadXmlFile(argv[1], specs), "editor XML parses");
        Check(specs.size() == 4, "three actions preserved, and the start's selecting action (format 5)");
        const auto* back = Find(specs, "BowDodgeBack");
        const auto* left = Find(specs, "BowDodgeLeft");
        const auto* right = Find(specs, "BowDodgeRight");
        Check(back && left && right, "actions found by id");
        for (const auto* s : {back, left, right}) Check(s->clips.size() == 1 && s->HasRecovery(), "one clip, recovery");
        Check(Near(back->recovery.at, 0.3f) && Near(back->recovery.blend, 0.1f), "back timing and blend");
        Check(left->recovery.event == "MoveReady" && left->recovery.at < 0 && Near(left->recovery.blend, 0.25f),
            "left annotation only");
        Check(Near(right->recovery.at, 1.1f) && Near(right->recovery.blend, 0.4f), "right timing and blend");
        Check(back->recovery.recoverEvent != left->recovery.recoverEvent &&
                  left->recovery.recoverEvent != right->recovery.recoverEvent,
            "each action has its own exit event");
        DeclarationCatalog catalog;
        catalog.Build(specs);
        for (const auto* s : {back, left, right}) {
            Check(std::find(catalog.events.begin(), catalog.events.end(), s->recovery.event) != catalog.events.end(),
                "annotation registered");
            Check(std::count(catalog.events.begin(), catalog.events.end(), s->recovery.recoverEvent) == 1,
                "exit registered once");
        }
        auto changed = *left;
        changed.recovery.enabled = false;
        Check(!changed.HasRecovery(), "an action without Recovery cannot cancel");
        std::cout << "Action recovery: parser, timing, blends, per-action exits and event catalog PASS\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
