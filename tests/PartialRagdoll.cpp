#include "core/PartialRagdollPolicy.h"
#include "runtime/CarryDrop.h"
#include <array>
#include <cstdlib>
#include <iostream>
#include <limits>

using namespace AnimationInjector::PartialRagdollPolicy;
int main() {
    unsigned checks = 0;
    auto check = [&](bool condition) {
        ++checks;
        if (!condition) {
            std::cerr << "Failed check " << checks << '\n';
            std::exit(1);
        }
    };
    // The tester's actual BipedNoCC matrix does not allow body self-collision.
    check(!LayerAllowsSelf(0x00001040400004F0ull, 33));
    check(LayerAllowsSelf(std::uint64_t{1} << 32, 32));
    check(!LayerAllowsSelf(std::uint64_t{1} << 8, 32));
    check(!LayerAllowsSelf(~std::uint64_t{0}, 64));
    // Someone else simulates a target body only while it is dynamic in the world and the target is knocked
    // down or getting up. A hit reaction's leftover on an upright target is taken, in or out of the world.
    check(SimulatedByOther(true, true, true, true));
    check(!SimulatedByOther(true, true, true, false));   // 0.12.243: 18/18 in the world, knock 0
    check(!SimulatedByOther(true, false, true, false));  // 0.12.240: outside the world
    check(!SimulatedByOther(true, false, true, true));
    check(!SimulatedByOther(true, true, false, true));
    check(!SimulatedByOther(false, true, true, true));
    // A taken body's frame: its own only when keyframed in the world (it follows the pose).
    check(!FrameFromBone(true, false));
    check(FrameFromBone(true, true));
    check(FrameFromBone(false, false));
    check(FrameFromBone(false, true));
    // Actual humanoid shape anchored at COM: every other owned body simulates.
    constexpr std::size_t ownedBodies = 18;
    struct Body {
        bool owned, selected;
    };
    std::array<Body, 19> bodies{};  // One extra equipped collision body is not ours.
    const std::vector<std::pair<std::size_t, std::size_t>> joints{{0, 1}, {1, 2}, {2, 3}, {0, 4}, {4, 5}, {5, 6},
        {0, 7}, {7, 8}, {8, 9}, {9, 10}, {10, 11}, {9, 12}, {12, 13}, {13, 14}, {9, 15}, {15, 16}, {16, 17}};
    {
        unsigned selected = 0;
        for (std::size_t i = 0; i < bodies.size(); ++i) {
            const bool owned = i < ownedBodies;
            bodies[i] = {owned, Selected(owned, i == 0)};
            selected += bodies[i].selected ? 1 : 0;
        }
        check(selected == 17u);
        check(!bodies[0].selected && !bodies[18].selected && bodies[7].selected);
        check(ConnectedToAnchors(bodies, joints));
        {
            auto disconnected = joints;
            disconnected.erase(disconnected.begin() + 11);
            check(!ConnectedToAnchors(bodies, disconnected));
            disconnected.push_back({18, 12});  // Equipment cannot rescue an isolated chain.
            check(!ConnectedToAnchors(bodies, disconnected));
            bodies[0].selected = true;
            check(!ConnectedToAnchors(bodies, joints));
            bodies[0].selected = false;
            auto invalidJoints = joints;
            invalidJoints.push_back({0, 99});
            check(!ConnectedToAnchors(bodies, invalidJoints));
        }
        std::array<float, 19> dampingValues;
        dampingValues.fill(.125f);
        std::array<DampingOverride, 19> overrides;
        for (std::size_t i = 0; i < bodies.size(); ++i)
            if (bodies[i].selected) check(overrides[i].Apply(dampingValues[i], 3.f));
        for (std::size_t i = 0; i < bodies.size(); ++i) check(dampingValues[i] == (bodies[i].selected ? 3.f : .125f));
        for (std::size_t i = 0; i < bodies.size(); ++i) overrides[i].Restore(dampingValues[i]);
        for (float value : dampingValues) check(value == .125f);
    }
    // Failure at every native operation, including a failure after side effects.
    for (unsigned failure = 0; failure < 18; ++failure) {
        std::array<int, 18> state{};
        unsigned undos = 0;
        check(!Transaction(
            18,
            [&](unsigned i) {
                state[i] = 1;
                return i != failure;
            },
            [&](unsigned i) {
                state[i] = 0;
                ++undos;
            }));
        check(undos == failure + 1);
        for (auto value : state) check(value == 0);
    }
    unsigned writes = 0, undos = 0;
    check(Transaction(
        8,
        [&](unsigned) {
            ++writes;
            return true;
        },
        [&](unsigned) { ++undos; }));
    check(writes == 8 && undos == 0);
    // Sync flags removed by us are restored; unrelated flags survive cleanup.
    check(RestoreBits(0x40, 0x480, 0, 0x480) == 0x4C0);
    check(RestoreBits(0x80, 0x480, 0, 0x480) == 0x480);
    check(RestoreBits(0x400, 0x80, 0, 0x480) == 0x480);
    // Reproduce an equipped weapon nested below a ragdoll arm. The native
    // setter recursively overwrites every descendant; ignored equipment must
    // retain its own layer, including changes from another system at cleanup.
    struct Node {
        bool owned;
        int parent, layer, original;
    };
    std::array<Node, 6> tree{
        {{true, -1, 1, 1}, {true, 0, 2, 2}, {false, 1, 7, 7}, {false, 2, 8, 8}, {true, 0, 3, 3}, {false, 4, 9, 9}}};
    const auto read = [](const Node& node) { return node.layer; };
    const auto recursiveWrite = [&](Node& node, int layer) {
        const auto index = static_cast<int>(&node - tree.data());
        node.layer = layer;
        for (int i = index + 1; i < static_cast<int>(tree.size()); ++i) {
            int parent = tree[i].parent;
            while (parent >= 0 && parent != index) parent = tree[parent].parent;
            if (parent == index) tree[i].layer = layer;
        }
    };
    check(ApplyLayerPlan(tree, [](const Node& n, int current) { return n.owned ? 0 : current; }, read, recursiveWrite));
    for (const auto& n : tree) check(n.layer == (n.owned ? 0 : n.original));
    tree[2].layer = 17;
    tree[3].layer = 18;
    tree[4].layer = 23;
    check(ApplyLayerPlan(
        tree, [](const Node& n, int current) { return n.owned && current == 0 ? n.original : current; }, read,
        recursiveWrite));
    check(tree[0].layer == 1 && tree[1].layer == 2 && tree[4].layer == 23);
    check(tree[2].layer == 17 && tree[3].layer == 18 && tree[5].layer == 9);
    check(!ApplyLayerPlan(tree, [](const Node&, int) { return 0; }, read, [](Node&, int) {}));
    // Native motion switches cache dynamic damping. Restore before caching it,
    // including repeated comparison toggles and partial activation failures.
    float savedDynamic = 0.125f;
    DampingOverride damping;
    for (unsigned cycle = 0; cycle < 8; ++cycle) {
        float live = savedDynamic;
        check(damping.Apply(live, 3.0f));
        check(live == 3.0f);
        check(!damping.Apply(live, 4.0f));  // Do not overwrite our original snapshot.
        damping.Restore(live);
        savedDynamic = live;
        check(savedDynamic == 0.125f && !damping.owned);
    }
    float strong = 5.0f;
    check(damping.Apply(strong, 3.0f));
    check(strong == 5.0f);
    damping.Restore(strong);
    check(strong == 5.0f);
    float external = 0.25f;
    check(damping.Apply(external, 3.0f));
    external = 1.5f;
    damping.Restore(external);
    check(external == 1.5f && !damping.owned);
    float linear = 0.125f, angular = std::numeric_limits<float>::quiet_NaN();
    DampingOverride linearOverride, angularOverride;
    check(linearOverride.Apply(linear, 0.25f));
    check(!angularOverride.Apply(angular, 3.0f));
    linearOverride.Restore(linear);
    angularOverride.Restore(angular);
    check(linear == 0.125f && !linearOverride.owned && !angularOverride.owned);
    float invalid = -1.0f;
    check(!damping.Apply(invalid, 3.0f));
    check(invalid == -1.0f && !damping.owned);
    // A quantizing native scalar requires readback, not comparison to the
    // requested float, otherwise cleanup incorrectly treats it as external.
    struct Quantized {
        float value = 0.125f;
        operator float() const { return value; }
        Quantized& operator=(float v) {
            value = std::floor(v * 8.0f) / 8.0f;
            return *this;
        }
    } quantized;
    check(damping.Apply(quantized, 3.14f));
    check(float(quantized) == 3.125f);
    damping.Restore(quantized);
    check(float(quantized) == 0.125f);
    // Native drop: restoring contacts can require several frames. Never request
    // knockdown early, never repeat it while waiting, and commit exactly once.
    using AnimationInjector::CarryDrop::NativeHandoff;
    using AnimationInjector::CarryDrop::HandoffResult;
    NativeHandoff handoff;
    unsigned restoreCalls = 0, requestCalls = 0, commitCalls = 0;
    bool controllerReady = false, nativeReady = false;
    auto restoreController = [&] {
        ++restoreCalls;
        return controllerReady;
    };
    auto requestNative = [&] {
        ++requestCalls;
        check(controllerReady);
        return true;
    };
    auto commitNative = [&] {
        ++commitCalls;
        check(controllerReady && requestCalls == 1);
        return nativeReady ? HandoffResult::kComplete : HandoffResult::kWaiting;
    };
    handoff.Start();
    for (unsigned i = 0; i < 3; ++i)
        check(handoff.Advance(.016f, restoreController, requestNative, commitNative) == HandoffResult::kWaiting);
    check(restoreCalls == 3 && requestCalls == 0 && commitCalls == 0);
    controllerReady = true;
    for (unsigned i = 0; i < 5; ++i)
        check(handoff.Advance(.016f, restoreController, requestNative, commitNative) == HandoffResult::kWaiting);
    check(restoreCalls == 4 && requestCalls == 1 && commitCalls == 5);
    nativeReady = true;
    check(handoff.Advance(.016f, restoreController, requestNative, commitNative) == HandoffResult::kComplete);
    check(!handoff.active && requestCalls == 1 && commitCalls == 6);
    check(handoff.Advance(.016f, restoreController, requestNative, commitNative) == HandoffResult::kFailed);
    check(requestCalls == 1 && commitCalls == 6);
    // Timeout before collision recovery must never launch a late knockdown.
    handoff.Start();
    controllerReady = false;
    restoreCalls = requestCalls = commitCalls = 0;
    for (unsigned i = 0; i < 25 && handoff.active; ++i)
        handoff.Advance(.1f, restoreController, requestNative, commitNative);
    check(!handoff.active && !handoff.requested && requestCalls == 0 && commitCalls == 0);
    controllerReady = true;
    check(handoff.Advance(.016f, restoreController, requestNative, commitNative) == HandoffResult::kFailed);
    check(requestCalls == 0);
    // Engine rejection/partial native transition is bounded without spamming.
    handoff.Start();
    nativeReady = false;
    restoreCalls = requestCalls = commitCalls = 0;
    for (unsigned i = 0; i < 25 && handoff.active; ++i)
        handoff.Advance(.1f, restoreController, requestNative, commitNative);
    check(!handoff.active && requestCalls == 1 && restoreCalls == 1 && commitCalls > 0);
    handoff.Start();
    requestCalls = commitCalls = 0;
    check(handoff.Advance(
              .016f, restoreController,
              [&] {
                  ++requestCalls;
                  return false;
              },
              commitNative) == HandoffResult::kFailed);
    check(!handoff.active && requestCalls == 1 && commitCalls == 0);
    handoff.Start();
    check(handoff.Advance(
              .016f, restoreController, [] { return true; }, [] { return HandoffResult::kFailed; }) ==
          HandoffResult::kFailed);
    check(!handoff.active);
    // Pause/bad delta cannot expire the handoff; load reset cannot reissue it.
    handoff.Start();
    controllerReady = false;
    requestCalls = 0;
    for (float dt : {0.f, -1.f, std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()})
        check(handoff.Advance(dt, restoreController, requestNative, commitNative) == HandoffResult::kWaiting);
    check(handoff.elapsed == 0 && requestCalls == 0);
    handoff = {};
    controllerReady = true;
    check(handoff.Advance(.016f, restoreController, requestNative, commitNative) == HandoffResult::kFailed);
    check(requestCalls == 0);
    std::cout << checks << " partial ragdoll policy checks passed\n";
}
