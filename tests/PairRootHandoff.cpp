#include "PCH.h"
#include "core/PairRootHandoff.h"
#include "runtime/PairStartAir.h"
#include <iostream>
#include <cstdlib>
using namespace AnimationInjector;
namespace S = PairRootHandoff;
void check(bool v, const char* message) {
    if (!v) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}
bool near(float a, float b) { return std::fabs(a - b) < 0.001f; }
const char* file = "Animations\\ComboTest1\\Grapple_Test.hkx";
int main() {
    RE::Actor a, b;
    b.id = 2;
    RE::NiAVObject parent, ra, rb;
    ra.parent = rb.parent = &parent;
    auto call = [&](int role, RE::Actor& actor, RE::NiAVObject& root, float y, unsigned seq, bool allowed = true,
                    unsigned request = 1) {
        root.local.translate.y = y;
        S::Update({1, request}, role, &actor, &root, file, 1.95f, 2, .01f, allowed, seq, true, 2.0f);
    };
    S::Reset();
    call(0, a, ra, -100, 1);
    call(1, b, rb, 80, 1);
    call(0, a, ra, -70, 2);
    call(1, b, rb, 50, 2);
    check(near(a.position.y, -30) && near(b.position.y, 30), "roles contaminated");
    const int writes = a.writes;
    call(0, a, ra, -70, 2);
    check(a.writes == writes, "duplicate sequence moved twice");
    call(0, a, ra, -35, 3);
    call(1, b, rb, 25, 3);
    call(0, a, ra, 0, 4);
    call(1, b, rb, 0, 4);
    check(near(a.position.y, -100) && near(b.position.y, 80), "two role endpoints");
    call(0, a, ra, -10, 5);
    check(near(a.position.y, -100), "completed role kept moving");
    // Reset never restores an old position or controller value.
    auto old = a.position;
    int wc = a.controller.writes;
    S::Reset();
    check(near(a.position.y, old.y) && a.controller.writes == wc, "reset wrote engine state");
    call(0, a, ra, -80, 1, true, 2);
    call(1, b, rb, 80, 1, true, 2);
    a.dead = true;
    call(0, a, ra, -50, 2, true, 2);
    call(1, b, rb, 50, 2, true, 2);
    check(near(a.position.y, old.y) && near(b.position.y, 110), "death stopped wrong role");
    a.dead = false;
    call(0, a, ra, -20, 3, true, 2);
    check(near(a.position.y, old.y), "dead session resumed");
    S::Reset();
    call(0, a, ra, -80, 1);
    a.ragdoll = true;
    call(0, a, ra, -40, 2);
    check(near(a.position.y, old.y), "ragdoll wrote position");
    a.ragdoll = false;
    S::Reset();
    call(0, a, ra, -80, 1);
    call(0, a, ra, -40, 2, false);
    check(near(a.position.y, old.y), "cancel wrote position");
    S::Reset();
    call(0, a, ra, -80, 1);
    S::Cancel(0);
    call(0, a, ra, -40, 2);
    check(near(a.position.y, old.y), "3D loss resumed");
    S::Reset();
    call(0, a, ra, 0, 1);
    call(0, a, ra, 0, 2);
    check(near(a.position.y, old.y), "stationary root moved");
    S::Reset();
    call(0, a, ra, -80, 1);
    a.position.x += 10;
    call(0, a, ra, -40, 2);
    check(near(a.position.y, old.y), "external movement overwritten");
    S::Reset();
    a.position = {};
    ra.local.translate = {0, -100, 0};
    S::Update({2, 1}, 0, &a, &ra, file, 0.1f, 10, .01f, true, 1, false, 4);
    ra.local.translate.y = -140;
    S::Update({2, 1}, 0, &a, &ra, file, 0.2f, 10, .01f, true, 2, false, 4);
    check(near(a.position.y, 0), "playing pose was treated as exit");
    ra.local.translate.y = -120;
    S::Update({2, 1}, 0, &a, &ra, file, 0.21f, 10, .01f, true, 3, true, 4);
    check(near(a.position.y, -20), "first early-exit blend step was lost");
    for (unsigned i = 1; i <= 300; ++i) {
        ra.local.translate.y = -120.0f * (1.0f - i / 300.0f);
        S::Update({2, 1}, 0, &a, &ra, file, .21f, 10, .01f, true, i + 3, true, 4);
    }
    check(near(a.position.y, -140), "three-second early exit did not complete");
    // Different nonzero rest poses must finish independently, including crossing zero.
    S::Reset();
    a.position = {};
    b.position = {};
    const RE::NiPoint3 refs[2] = {{0, 0.136101f, 0}, {0, -2.545135f, 0}};
    for (unsigned step = 0; step <= 20; ++step) {
        for (int role = 0; role < 2; ++role) {
            auto& actor = role == 0 ? a : b;
            auto& root = role == 0 ? ra : rb;
            root.local.translate = refs[role];
            root.local.translate.y += (role == 0 ? -10.0f : 10.0f) * (1.0f - step / 20.0f);
            S::Update({3, 1}, role, &actor, &root, file, 1, 2, .01f, true, step + 1, true, 2, refs[role]);
        }
    }
    check(near(a.position.y, -10) && near(b.position.y, 10), "creature rest offsets lost displacement");
    check(!S::Busy({3, 1}), "nonzero rest pose did not complete");
    S::Reset();
    int before = a.writes;
    ra.local.translate = refs[0];
    S::Update({3, 2}, 0, &a, &ra, file, 1, 2, .01f, true, 1, true, 2, refs[0]);
    check(a.writes == before && !S::Busy({3, 2}), "rest pose was treated as authored motion");

    // An explicitly cancelled exit may collapse the remaining offset in one copy.
    S::Reset();
    a.position = {};
    b.position = {};
    auto abrupt = [&](int role, RE::Actor& actor, RE::NiAVObject& root, float y, unsigned seq, bool exiting,
                      bool cancel, unsigned request = 1) {
        root.local.translate = {0, y, 0};
        S::Update({4, request}, role, &actor, &root, file, 1, 2, .01f, true, seq, exiting, 4, {}, cancel);
    };
    abrupt(0, a, ra, -440, 1, false, false);
    abrupt(1, b, rb, 440, 1, false, false);
    abrupt(0, a, ra, -430, 2, true, false);
    abrupt(0, a, ra, -430, 2, true, true);  // Intent can arrive before the next pose copy.
    abrupt(0, a, ra, -425, 3, true, true);  // A final normal copy must not consume the large-step allowance.
    abrupt(0, a, ra, 0, 4, true, true);
    check(near(a.position.y, -440), "movement cancellation lost remaining root displacement");
    int once = a.writes;
    abrupt(0, a, ra, 0, 5, true, true);
    check(a.writes == once, "cancel applied twice");
    abrupt(1, b, rb, 0, 2, true, false);
    check(near(b.position.y, 0), "cancel permission leaked to other role");
    // Either paired role can be the player; regular exits retain their step guard.
    S::Reset();
    a.position = {};
    b.position = {};
    abrupt(1, b, rb, 440, 1, false, false);
    abrupt(1, b, rb, 0, 2, true, true);
    check(near(b.position.y, 440), "target-role movement cancellation failed");
    abrupt(0, a, ra, -440, 1, false, false, 2);
    abrupt(0, a, ra, 0, 2, true, false, 2);
    check(near(a.position.y, 0), "new session inherited cancellation allowance");
    S::Reset();
    a.position = {};
    abrupt(0, a, ra, -600, 1, false, false);
    abrupt(0, a, ra, 0, 2, true, true);
    check(near(a.position.y, 0), "cancel bypassed maximum root offset guard");
    S::Reset();
    a.position = {};
    abrupt(0, a, ra, -440, 1, false, false);
    a.position.x = 20;
    abrupt(0, a, ra, 0, 2, true, true);
    check(near(a.position.y, 0), "cancel bypassed external movement guard");

    // A pair linked straight after another starts with the controller the old exit left in
    // air; that state stays frozen while the new pair holds the actor.
    using PairStartAir::Exit;
    check(PairStartAir::Inherited(true, true, Exit{true, 0.2f}), "linked pair air not inherited");
    check(!PairStartAir::Inherited(false, true, Exit{true, 0.2f}), "grounded start marked inherited");
    check(!PairStartAir::Inherited(true, false, Exit{true, 0.2f}), "another actor's exit inherited");
    check(!PairStartAir::Inherited(true, true, Exit{false, 0.2f}), "airborne pair's exit treated as ground");
    check(!PairStartAir::Inherited(true, true, Exit{true, 1.5f}), "stale exit inherited");
    check(!PairStartAir::Inherited(true, true, Exit{}), "air without a previous pair inherited");
    auto held = [&](float y, unsigned seq, bool inherited) {
        ra.local.translate = {0, y, 0};
        S::Update({5, 1}, 0, &a, &ra, file, 1.95f, 2, .01f, true, seq, true, 2.0f, {}, false, false, inherited);
    };
    S::Reset();
    a.position = {};
    a.air = true;
    held(-80, 1, true);
    held(-40, 2, true);
    check(near(a.position.y, -40), "inherited air stopped the handoff");
    S::Reset();
    a.position = {};
    held(-80, 1, false);
    held(-40, 2, false);
    check(near(a.position.y, 0), "real air wrote position");
    a.air = false;

    // A kill move the engine started (NativePairEnd, traced in game 2026-10-03): the clip walks the root to -110,
    // the frozen controller reports "in air" for one frame before the end, the end begins in the clip's last
    // 0.1 s, the root holds, then blends back about 8 a frame. The actor ends where the root ended.
    auto native = [&](bool ignoreAir) {
        S::State state;
        a.position = {};
        a.air = false;
        std::uint64_t sample = 0;
        auto frame = [&](float y, bool ending) {
            ra.local.translate = {0, y, 0};
            S::Update(state, 7, 0, S::Frame{&a, &ra, {}, .01f, true, ++sample, ending, 2.0f, false, true, ignoreAir});
        };
        for (float y = 0; y > -110; y -= 2.75f) frame(y, false);
        a.air = true;  // one frame, 0.3 s before the end
        frame(-109.91f, false);
        a.air = false;
        for (int i = 0; i < 30; ++i) frame(-109.91f, false);
        for (int i = 0; i < 10; ++i) frame(-109.91f, true);  // the last 0.1 s and the held end pose
        for (float y = -109.91f; y < 0; y += 8.0f) frame(y, true);
        frame(0, true);
        return state;
    };
    const auto kept = native(true);
    check(kept.stopped && near(a.position.y, -109.91f), "native kill move lost its end position");
    const auto strict = native(false);
    check(strict.stopped && near(a.position.y, 0), "the frozen air frame did not stop the strict handoff");
    // The air exemption must not weaken the other interruption guards (native and
    // AI policy).
    for (const bool inherited : {false, true}) {
        for (int reason = 0; reason < 4; ++reason) {
            S::State state;
            RE::Actor actor;
            ra.local.translate = {0, -80, 0};
            S::Update(state, 10, 0, S::Frame{&actor, &ra, {}, .01f, true, 1, false, 2, false, true, inherited});
            actor.dead = reason == 0;
            actor.ragdoll = reason == 1;
            actor.state.knock = reason == 2 ? 1 : 0;
            ra.local.translate.y = -40;
            S::Update(state, 10, 0, S::Frame{&actor, &ra, {}, .01f, reason != 3, 2, true, 2, false, true, inherited});
            check(state.stopped && actor.writes == 0 && actor.controller.writes == 0,
                "air exemption bypassed death/ragdoll/knock/cancellation");
        }
    }
    // Once complete, later frames cannot reapply the displacement; a new owner
    // can start normally.
    for (const bool inherited : {false, true}) {
        S::State state;
        RE::Actor actor;
        auto frame = [&](float y, unsigned owner, unsigned sample) {
            ra.local.translate = {0, y, 0};
            S::Update(state, owner, 0, S::Frame{&actor, &ra, {}, .01f, true, sample, true, 2, false, true, inherited});
        };
        frame(-80, 11, 1);
        frame(-40, 11, 2);
        frame(0, 11, 3);
        const int completedWrites = actor.writes;
        for (unsigned i = 4; i < 355; ++i) frame(i % 2 ? -80.0f : 0.0f, 11, i);
        check(near(actor.position.y, -80) && actor.writes == completedWrites,
            "completed handoff reapplied during 3.5 seconds of later frames");
        frame(-40, 12, 1);
        frame(0, 12, 2);
        check(state.stopped && near(actor.position.y, -120), "new owner inherited stopped state");
    }
    // World-space direction and scale must follow each participant's parent transform.
    for (const float yaw : {0.0f, 1.570796327f, 3.141592654f, -1.570796327f}) {
        for (const float scale : {1.0f, 1.25f}) {
            for (const float offset : {-80.0f, 80.0f}) {
                S::State state;
                RE::Actor actor;
                parent.world.rotate.yaw = yaw;
                parent.world.scale = scale;
                for (unsigned step = 0; step <= 4; ++step) {
                    ra.local.translate = {0, offset * (1.0f - step / 4.0f), 0};
                    S::Update(
                        state, 13, 0, S::Frame{&actor, &ra, {}, .01f, true, step + 1, true, 2, false, true, true});
                }
                check(state.stopped && near(actor.position.x, -std::sin(yaw) * offset * scale) &&
                          near(actor.position.y, std::cos(yaw) * offset * scale),
                    "rotated or scaled participant lost its signed end displacement");
            }
        }
    }
    parent.world = {};
    // Engine-authored movement with no residual local root is already in actor position.
    // A root handoff must neither add that movement again nor restore the pair's start.
    for (const bool inherited : {false, true}) {
        S::State state;
        RE::Actor actor;
        ra.local.translate = {};
        for (unsigned step = 1; step <= 8; ++step) {
            actor.position = {step * 10.0f, step * -15.0f, 0};
            S::Update(state, 14, 0, S::Frame{&actor, &ra, {}, .01f, true, step, step >= 5, 2, false, true, inherited});
        }
        check(actor.writes == 0 && actor.controller.writes == 0 && near(actor.position.y, -120),
            "root-neutral engine movement was applied twice or restored");
    }
    // Locomotion during/after a fade is additive; no historical actor anchor is restored.
    for (const bool inherited : {false, true}) {
        S::State state;
        RE::Actor actor;
        ra.local.translate = {0, -80, 0};
        S::Update(state, 15, 0, S::Frame{&actor, &ra, {}, .01f, true, 1, false, 2, false, true, inherited});
        for (unsigned step = 1; step <= 4; ++step) {
            actor.position.x += 5;
            actor.position.y += 3;
            actor.yaw += .2f;
            ra.local.translate.y = -80.0f + step * 20.0f;
            S::Update(state, 15, 0, S::Frame{&actor, &ra, {}, .01f, true, step + 1, true, 2, false, true, inherited});
        }
        check(state.stopped && near(actor.position.x, 20) && near(actor.position.y, -68),
            "locomotion during fade lost input or root displacement");
        const int completedWrites = actor.writes;
        actor.position.x += 7;
        actor.position.y += 9;
        S::Update(state, 15, 0, S::Frame{&actor, &ra, {}, .01f, true, 6, true, 2, false, true, inherited});
        check(actor.writes == completedWrites && near(actor.position.x, 27) && near(actor.position.y, -59),
            "ordinary movement after completion was overwritten");
    }
    std::cout
        << "PairRootHandoff: role isolation, duplicate samples, completion, reset, death, ragdoll, cancellation, 3D loss, zero displacement, external movement, playing samples, early exit, long blend, inherited air and a native kill move end passed\n";
}
