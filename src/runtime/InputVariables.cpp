#include "runtime/InputVariables.h"
#include "runtime/ActionRegistry.h"
#include "runtime/GraphVariables.h"
#include "runtime/InputSink.h"
#include "runtime/TdmMovement.h"

namespace AnimationInjector::InputVariables {

namespace {
// One source's variables as engine strings, made once on the main thread (global rule 4-3, principle 3), and what
// the player's graph holds for them (as last written successfully); -1 unknown.
struct Group {
    std::vector<RE::BSFixedString> names;
    int written = -1;
    bool refusedLogged = false;
};
Group g_move, g_strafe;
bool g_built = false;

void Build() {
    if (std::exchange(g_built, true)) return;
    for (const auto& v : Actions::RegisteredVariables()) {
        if (v.source == GraphVariables::Source::kMoveInput) g_move.names.emplace_back(v.name.c_str());
        if (v.source == GraphVariables::Source::kStrafeInput) g_strafe.names.emplace_back(v.name.c_str());
    }
    if (!g_move.names.empty() || !g_strafe.names.empty())
        logger::info("[InputVariable] {} variables follow the player's movement input, {} its strafing input",
            g_move.names.size(), g_strafe.names.size());
}
void Write(RE::PlayerCharacter* player, Group& group, int value, const char* what) {
    if (group.names.empty() || value == group.written) return;
    bool all = true;
    for (const auto& name : group.names) all = player->SetGraphVariableInt(name, value) && all;
    if (all) {
        group.written = value;
        group.refusedLogged = false;
    } else if (!std::exchange(group.refusedLogged, true)) {
        // The active graph lacks one (first person): tried again each frame.
        logger::info("[InputVariable] {} {} refused by the active graph; tried again", what, value);
    }
}
}  // namespace

void Update() {
    Build();
    if (g_move.names.empty() && g_strafe.names.empty()) return;
    auto* player = RE::PlayerCharacter::GetSingleton();
    if (!player || !player->Is3DLoaded()) return;
    const int keys = GraphVariables::MoveInputValue(static_cast<int>(InputSink::Get()->CurrentMoveDir(MoveDir::kNone)));
    Write(player, g_move, keys, "movement input");
    // The keys turn the character in True Directional Movement's directional setting: no strafing there. The
    // setting is the one in effect outside our actions (Actions::TdmMode), so it holds through an action, during
    // which TDM itself sees no movement input and reports directional whatever the setting.
    Write(player, g_strafe, Actions::TdmMode() == TdmMovement::Mode::kDirectional ? 0 : keys, "strafing input");
}

void Reset() {
    g_move.written = g_strafe.written = -1;
    g_move.refusedLogged = g_strafe.refusedLogged = false;
}

}  // namespace AnimationInjector::InputVariables
