#include "PCH.h"
#include "runtime/PairStartCamera.h"
#include "runtime/PairStartTransfer.h"
#include <cstdlib>
#include <iostream>
#include <limits>

namespace {
unsigned calls = 0;
bool lastSheathed = false;
void Check(bool ok, const char* why) {
    if (!ok) {
        std::cerr << why << '\n';
        std::exit(1);
    }
}
void ExistingCameraChain(RE::ThirdPersonState* state, bool sheathed) {
    ++calls;
    lastSheathed = sheathed;
    if (state) {
        state->freeRotationEnabled = sheathed;
        state->freeRotation.x += .01f;
    }
}
}
int main() {
    using namespace AnimationInjector::PairStartCamera;
    Test::cameraVfunc = ExistingCameraChain;
    Install();
    Install();
    Check(Test::installs == 1, "guard must preserve chain and install once");
    RE::ThirdPersonState state;
    RE::NiAVObject originalScene, replacedScene;
    RE::Actor player, npc;
    player.player = true;
    player.scene = &originalScene;
    npc.scene = &originalScene;
    RE::PlayerCamera camera;
    camera.currentState = RE::NiPointer<RE::ThirdPersonState>{&state};
    camera.cameraTarget = player.GetHandle();
    RE::PlayerCamera::instance = &camera;
    const auto ordinary = [&] {
        RE::actorQueries = RE::cameraQueries = 0;
        for (unsigned i = 0; i < 300; ++i) {
            const auto before = calls;
            const float orbit = state.freeRotation.x;
            const bool sheathed = i % 2 == 0;
            Test::cameraVfunc(&state, sheathed);
            Check(calls == before + 1 && lastSheathed == sheathed,
                "ordinary movement forwards each call and the exact weapon state");
            Check(state.freeRotationEnabled == sheathed && std::abs(state.freeRotation.x - (orbit + .01f)) < .0001f,
                "ordinary movement preserves all downstream camera changes");
        }
        Check(RE::actorQueries == 0 && RE::cameraQueries == 0,
            "no active pair must not touch actors, graphs or camera singleton");
    };
    ordinary();
    const auto token = Acquire(&player, &npc);
    Check(token != 0, "confirmed pair acquires startup guard");
    Check(Acquire(&npc, &player) == 0, "overlapping pair cannot steal camera ownership");
    auto before = calls;
    const float orbit = state.freeRotation.x;
    Test::cameraVfunc(&state, false);
    Check(calls == before && state.freeRotationEnabled && state.freeRotation.x == orbit,
        "owned pair keeps live orbit without downstream yaw reset");
    state.toggleAnimCam = true;
    Test::cameraVfunc(&state, false);
    Check(calls == before + 1, "animated camera still forwards during owned startup");
    state.toggleAnimCam = false;
    player.scene = &replacedScene;
    before = calls;
    Test::cameraVfunc(&state, true);
    Check(calls == before + 1, "replaced actor scene invalidates guard");
    player.scene = &originalScene;
    player.dead = true;
    before = calls;
    Test::cameraVfunc(&state, true);
    Check(calls == before + 1, "dead actor camera is not overridden");
    player.dead = false;
    player.ragdoll = true;
    before = calls;
    Test::cameraVfunc(&state, true);
    Check(calls == before + 1, "ragdoll camera is not overridden");
    player.ragdoll = false;
    camera.cameraTarget = npc.GetHandle();
    before = calls;
    Test::cameraVfunc(&state, true);
    Check(calls == before + 1, "different camera target is not overridden");
    camera.cameraTarget = player.GetHandle();
    state.freeRotation.x = std::numeric_limits<float>::quiet_NaN();
    before = calls;
    Test::cameraVfunc(&state, true);
    Check(calls == before + 1, "invalid orbit cannot be protected");
    state.freeRotation.x = 2;
    Release(token + 1);
    before = calls;
    Test::cameraVfunc(&state, false);
    Check(calls == before, "unrelated release cannot remove startup protection");
    Release(token);
    ordinary();
    const auto second = Acquire(&npc, &player);
    Check(second && second != token, "player victim can acquire a fresh guard");
    Release(token);
    before = calls;
    Test::cameraVfunc(&state, false);
    Check(calls == before, "stale owner cannot release the next pair");
    Release(second);
    ordinary();
    // Exercise the production ownership transfer with the actual camera hook.
    // Carry cleanup and native cleanup can happen in either order.
    for (bool nativeFirst : {false, true}) {
        auto carryToken = Acquire(&player, &npc);
        const auto expected = carryToken;
        Check(
            AnimationInjector::PairStartTransfer::ClaimCamera(carryToken, expected + 1) == 0 && carryToken == expected,
            "stale transfer retains current carry guard");
        auto nativeToken = AnimationInjector::PairStartTransfer::ClaimCamera(carryToken, expected);
        Check(nativeToken == expected && carryToken == 0, "native start takes exact carry guard without reacquiring");
        Check(AnimationInjector::PairStartTransfer::ClaimCamera(carryToken, expected) == 0,
            "duplicate transfer cannot own the same guard twice");
        before = calls;
        Test::cameraVfunc(&state, false);
        Check(calls == before, "there is no unguarded frame during handoff");
        if (nativeFirst) Release(nativeToken);
        Release(carryToken);
        before = calls;
        Test::cameraVfunc(&state, false);
        Check(calls == before + (nativeFirst ? 1 : 0), "carry cleanup cannot release transferred native guard");
        if (!nativeFirst) Release(nativeToken);
        auto next = Acquire(&player, &npc);
        Check(next && next != expected, "normal completion or rejection releases handoff ownership");
        Release(expected);
        before = calls;
        Test::cameraVfunc(&state, false);
        Check(calls == before, "old native cleanup cannot release a later carry");
        Release(next);
    }
    before = calls;
    Test::cameraVfunc(nullptr, false);
    Check(calls == before + 1, "null state passes through safely");
    std::cout
        << "Production camera hook: 900 ordinary movement calls, zero unowned actor queries, pair ownership/cleanup and downstream preservation passed\n";
}
