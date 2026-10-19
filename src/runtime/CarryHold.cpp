#include "PCH.h"
#include "runtime/CarryHold.h"
#include "core/GraphBoneSlot.h"
#include "runtime/CarryHoldMath.h"
#include "runtime/CarrySpec.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>
#include <optional>
#include <vector>

namespace AnimationInjector::CarryHold {
namespace {
// One graph-driven bone: a plain skeleton node, or an entry of a flattened bone tree.
struct Bone {
    RE::BSFixedString name;
    RE::NiPointer<RE::NiAVObject> owner;
    std::int32_t entry = -1;  // flattened tree entry, or -1
    std::int32_t index = -1;  // graph bone index, also the pose track's bone index
    float weight = 0;
    RE::NiTransform hold;
    RE::NiTransform start;  // the last playing sample: where the arm is drawn when the hold begins
};
enum class Mode : std::uint8_t { kIdle, kResolved, kHolding, kReleasing };
struct Resolved {
    Result result = Result::kNoGraph;
    RE::NiPointer<RE::NiAVObject> scene;
    std::vector<Bone> bones;
    std::size_t graphBones = 0;
};
struct State {
    Mode mode = Mode::kIdle;
    std::vector<std::pair<RE::BSFixedString, float>> config;
    RE::NiPointer<RE::NiAVObject> scene;
    std::vector<Bone> bones;
    float blendIn = 0.2f, blendOut = 0.2f, releaseBlend = 0.2f, fade = 0;
    bool captured = false;   // a hold pose is present
    bool clipEnd = false;    // it is the lift clip's last frame, not the last playing sample
    bool endTried = false;   // the clip end was sampled once (successfully or not)
    bool sampled = false;    // every bone has a last playing sample (Bone::start)
    bool fromStart = false;  // blending in from Bone::start at full weight (CarryHoldMath::Held)
    std::uint64_t samples = 0, writes = 0;
};
std::mutex mutex;
State state;
std::atomic_bool writing{false};

bool Finite(const RE::NiTransform& t) {
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            if (!std::isfinite(t.rotate.entry[r][c])) return false;
    return std::isfinite(t.translate.x) && std::isfinite(t.translate.y) && std::isfinite(t.translate.z) &&
           std::isfinite(t.scale);
}
// Havok stores (x, y, z, w); the same convention as the rigid-body writes in CarryPartialRagdoll.
RE::NiTransform ToNi(const RE::hkQsTransform& t) {
    RE::NiTransform out;
    const auto& p = t.translation.quad.m128_f32;
    const auto& q = t.rotation.vec.quad.m128_f32;
    out.translate = {p[0], p[1], p[2]};
    RE::NiQuaternion(q[3], q[0], q[1], q[2]).ToRotation(out.rotate);
    out.scale = t.scale.quad.m128_f32[0];
    return out;
}
GraphBoneSlot::Slot Access(const Bone& bone) { return GraphBoneSlot::Access(bone.owner.get(), bone.entry); }
// Main thread, without our mutex: the graph lock must never nest inside it.
Resolved Resolve(RE::Actor* carrier, const std::vector<std::pair<RE::BSFixedString, float>>& config) {
    Resolved out;
    auto* scene = carrier ? carrier->Get3D(false) : nullptr;
    RE::BSTSmartPointer<RE::BSAnimationGraphManager> manager;
    if (!scene || !carrier->GetAnimationGraphManager(manager) || !manager) return out;
    struct Entry {
        RE::BSFixedString name;
        RE::NiAVObject* owner = nullptr;
        std::int32_t entry = -1, parent = -1;
    };
    std::vector<Entry> graph;
    {
        RE::BSSpinLockGuard lock(manager->GetRuntimeData().updateLock);
        const auto active = manager->GetRuntimeData().activeGraph;
        if (active >= manager->graphs.size() || !manager->graphs[active]) return out;
        for (const auto& bone : manager->graphs[active]->boneNodes) {
            Entry e{{}, bone.node, static_cast<std::int32_t>(bone.unk08)};
            if (e.entry >= 0) {
                auto* tree = netimmerse_cast<RE::BSFlattenedBoneTree*>(bone.node);
                if (!tree) return out;
                const auto& data = tree->GetRuntimeData();
                if (!data.boneEntries || static_cast<std::uint32_t>(e.entry) >= data.numBones) return out;
                e.name = data.boneEntries[e.entry].nodeName;
            } else if (bone.node)
                e.name = bone.node->name;
            graph.push_back(e);
        }
    }
    out.graphBones = graph.size();
    const auto find = [&](const RE::BSFixedString& name) {
        for (std::size_t i = 0; i < graph.size(); ++i)
            if (!name.empty() && graph[i].name == name) return static_cast<int>(i);
        return -1;
    };
    // Nearest ancestor that the graph also drives.
    for (auto& e : graph) {
        if (e.entry >= 0) {
            const auto& data = netimmerse_cast<RE::BSFlattenedBoneTree*>(e.owner)->GetRuntimeData();
            for (auto i = data.boneEntries[e.entry].parentIndex; i >= 0 && e.parent < 0;
                i = data.boneEntries[i].parentIndex)
                e.parent = find(data.boneEntries[i].nodeName);
        } else if (e.owner)
            for (auto* node = e.owner->parent; node && e.parent < 0; node = node->parent) e.parent = find(node->name);
    }
    for (const auto& [name, weight] : config)
        if (find(name) < 0) {
            logger::warn(
                "[CarryHold] bone '{}' is not driven by the carrier's animation graph; hold disabled", name.c_str());
            out.result = Result::kUnknownBone;
            return out;
        }
    std::vector<float> weights(graph.size());
    for (std::size_t i = 0; i < graph.size(); ++i)
        weights[i] = CarryHoldMath::Inherited(
            static_cast<int>(i), [&](int b) { return graph[b].parent; },
            [&](int b) -> std::optional<float> {
                for (const auto& [name, weight] : config)
                    if (graph[b].name == name) return weight;
                return std::nullopt;
            });
    for (std::size_t i = 0; i < graph.size(); ++i)
        if (weights[i] > 0 && graph[i].owner)
            out.bones.push_back({graph[i].name, RE::NiPointer<RE::NiAVObject>(graph[i].owner), graph[i].entry,
                static_cast<std::int32_t>(i), weights[i], {}});
    out.scene.reset(scene);
    out.result = out.bones.empty() ? Result::kDisabled : Result::kReady;
    return out;
}
void Publish(Resolved&& resolved) {
    // Carry sampled hold values across a re-resolve by bone name.
    for (auto& bone : resolved.bones)
        for (const auto& old : state.bones)
            if (old.name == bone.name) bone.hold = old.hold;
    state.scene = std::move(resolved.scene);
    state.bones = std::move(resolved.bones);
}
void StopWriting(const char* reason) {
    if (state.mode == Mode::kHolding || state.mode == Mode::kReleasing)
        logger::info("[CarryHold] stopped reason={} writes={}", reason, state.writes);
    state.mode = state.bones.empty() ? Mode::kIdle : Mode::kResolved;
    state.fade = 0;
    writing.store(false, std::memory_order_release);
}
// End of the player's animation step: the engine has just written the graph pose into the
// bone locals, and computes world transforms from them before the next frame. Writing the
// locals here keeps the hold in every later world update, including paused (menu) frames.
void Apply(RE::PlayerCharacter* player, float delta) {
    std::lock_guard lock(mutex);
    if (state.mode != Mode::kHolding && state.mode != Mode::kReleasing) return;
    auto* scene = player ? player->Get3D(false) : nullptr;
    if (!scene || scene != state.scene.get()) return StopWriting("3d-changed");
    if (player->IsDead() || player->IsInRagdollState() ||
        player->AsActorState()->GetKnockState() != RE::KNOCK_STATE_ENUM::kNormal)
        return StopWriting("carrier-not-standing");
    delta = std::isfinite(delta) ? std::clamp(delta, 0.0f, 0.1f) : 0.0f;
    if (state.mode == Mode::kReleasing) {
        state.fade = state.releaseBlend > 0 ? state.fade - delta / state.releaseBlend : 0;
        if (state.fade <= 0) return StopWriting("released");
    } else if (state.fade < 1)
        state.fade = state.blendIn > 0 ? std::min(1.0f, state.fade + delta / state.blendIn) : 1;
    const bool fromStart = state.mode == Mode::kHolding && state.fromStart;
    for (const auto& bone : state.bones) {
        const auto slot = Access(bone);
        if (!slot.local) continue;
        const auto next = CarryHoldMath::Held<RE::NiTransform, RE::NiQuaternion>(*slot.local,
            fromStart ? std::optional<RE::NiTransform>(bone.start) : std::nullopt, bone.hold, bone.weight, state.fade);
        if (!Finite(next)) return StopWriting("non-finite");
        *slot.local = next;
        if (slot.mirror) *slot.mirror = next;
    }
    if (++state.writes == 1)
        logger::info("[CarryHold] first write bones={} fade={:.2f}", state.bones.size(), state.fade);
}
struct AnimationHook {
    static void thunk(RE::PlayerCharacter* player, float delta) {
        original(player, delta);
        if (writing.load(std::memory_order_acquire)) Apply(player, delta);
    }
    static inline REL::Relocation<decltype(thunk)> original;
};
}
void Install() {
    if (REL::Module::IsVR()) {
        logger::info("[CarryHold] VR: hold pose unavailable (vtable index not mapped)");
        return;
    }
    REL::Relocation<std::uintptr_t> table{RE::VTABLE_PlayerCharacter[0]};
    AnimationHook::original = table.write_vfunc(0x7D, AnimationHook::thunk);
    logger::info("[CarryHold] installed at PlayerCharacter::UpdateAnimation exit");
}
Result Begin(RE::ActorHandle carrier, const CarryHoldSpec& spec) {
    Reset();
    if (!spec.Enabled()) return Result::kDisabled;
    std::vector<std::pair<RE::BSFixedString, float>> config;
    for (const auto& bone : spec.bones) config.emplace_back(RE::BSFixedString(bone.name.c_str()), bone.weight);
    // Carry only starts from the player (Carry::CanBegin). Handles are resolved here, never
    // under our mutex, which the animation step also takes.
    auto actor = carrier.get();
    if (!actor || !actor->IsPlayerRef()) return Result::kNoGraph;
    auto resolved = Resolve(actor.get(), config);
    const auto result = resolved.result;
    const auto graphBones = resolved.graphBones;
    std::lock_guard lock(mutex);
    if (result != Result::kReady) {
        logger::info("[CarryHold] begin failed result={} graphBones={}", static_cast<int>(result), graphBones);
        return result;
    }
    state.config = std::move(config);
    state.blendIn = spec.blendIn;
    state.blendOut = spec.blendOut;
    Publish(std::move(resolved));
    state.mode = Mode::kResolved;
    std::size_t flattened = 0;
    for (const auto& bone : state.bones) flattened += bone.entry >= 0;
    logger::info("[CarryHold] begin source=lift-end held={} flattened={} graphBones={} blendIn={:.2f} blendOut={:.2f}",
        state.bones.size(), flattened, graphBones, state.blendIn, state.blendOut);
    return Result::kReady;
}
void SampleClip(const RE::hkbClipGenerator& clip) {
    std::lock_guard lock(mutex);
    if (state.mode != Mode::kResolved || state.endTried) return;
    auto* control = clip.animationControl.get();
    auto* binding = control ? control->binding : nullptr;
    auto* animation = binding ? binding->animation.get() : nullptr;
    if (!animation || binding->blendHint.get() != RE::hkaAnimationBinding::BlendHint::kNormal) return;
    const auto tracks = animation->numberOfTransformTracks;
    if (tracks <= 0 || tracks > 1024 || !(animation->duration > 0)) return;
    // Track of each held bone; an empty map means track i drives bone i.
    const auto& map = binding->transformTrackToBoneIndices;
    std::vector<int> trackOf(state.bones.size(), -1);
    for (int t = 0; t < tracks; ++t) {
        const int bone = map._size ? (t < map._size && map._data ? map._data[t] : -1) : t;
        for (std::size_t i = 0; i < state.bones.size(); ++i)
            if (state.bones[i].index == bone) trackOf[i] = t;
    }
    std::size_t mapped = 0;
    for (const int t : trackOf) mapped += t >= 0;
    std::vector<RE::hkQsTransform> buffer(static_cast<std::size_t>(tracks));
    const auto sample = [&](float time, std::vector<RE::NiTransform>& out) {
        if (mapped != state.bones.size()) return false;
        animation->SamplePartialTracks(time, static_cast<std::uint32_t>(tracks), buffer.data(), 0, nullptr, nullptr);
        out.resize(state.bones.size());
        for (std::size_t i = 0; i < out.size(); ++i) {
            out[i] = ToNi(buffer[static_cast<std::size_t>(trackOf[i])]);
            if (!Finite(out[i])) return false;
        }
        return true;
    };
    state.endTried = true;
    std::vector<RE::NiTransform> end;
    // A lift cut short by movement recovery still holds the pose its clip ends in.
    if (sample(std::max(0.0f, animation->duration - 0.001f), end)) {
        for (std::size_t i = 0; i < end.size(); ++i) state.bones[i].hold = end[i];
        state.clipEnd = state.captured = true;
    }
    logger::info("[CarryHold] clip end duration={:.3f} tracks={} mapped={}/{} used={}", animation->duration, tracks,
        mapped, state.bones.size(), state.clipEnd);
}
Result CaptureLiftEnd() {
    auto* player = RE::PlayerCharacter::GetSingleton();
    std::lock_guard lock(mutex);
    if (state.mode != Mode::kResolved) return Result::kPoseUnavailable;
    if (!player || player->Get3D(false) != state.scene.get()) {
        if (!state.clipEnd) state.captured = false;
        return Result::kNoGraph;
    }
    std::vector<RE::NiTransform> sample;
    sample.reserve(state.bones.size());
    for (const auto& bone : state.bones) {
        const auto slot = Access(bone);
        if (!slot.world || !slot.parent) return Result::kNoGraph;
        // Derived from the same world transforms the target's anchor sample reads.
        const auto local = slot.parent->Invert() * *slot.world;
        if (!Finite(local)) return Result::kNoGraph;
        sample.push_back(local);
    }
    ++state.samples;
    // The arm as drawn now: where the hold starts from once the lift ends.
    for (std::size_t i = 0; i < sample.size(); ++i) state.bones[i].start = sample[i];
    state.sampled = true;
    // Fallback only: the clip's last frame is the hold pose whenever it could be sampled.
    if (state.clipEnd) return Result::kReady;
    for (std::size_t i = 0; i < sample.size(); ++i) state.bones[i].hold = sample[i];
    state.captured = true;
    return Result::kReady;
}
void Hold() {
    auto* player = RE::PlayerCharacter::GetSingleton();
    if (!player) return;
    std::vector<std::pair<RE::BSFixedString, float>> config;
    {
        std::lock_guard lock(mutex);
        if (state.mode == Mode::kIdle || !state.captured) return;
        if (state.mode == Mode::kHolding) return;
        if (player->Get3D(false) == state.scene.get()) {
            // The clip's last frame blends in (the lift may have been cut short) from the last
            // playing sample, held at full weight all the way (CarryHoldMath::Held); the last
            // playing sample itself is where the arm already is.
            state.fade = state.clipEnd ? 0.0f : 1.0f;
            state.fromStart = state.clipEnd && state.sampled;
            state.mode = Mode::kHolding;
            writing.store(true, std::memory_order_release);
            logger::info("[CarryHold] hold source={} from={} samples={} bones={}",
                state.clipEnd ? "clip-end" : "last-sample", state.fromStart ? "last-sample" : "animation",
                state.samples, state.bones.size());
            return;
        }
        config = state.config;
    }
    // The carrier's 3D was replaced (transfer): re-resolve, keeping the sampled pose.
    auto resolved = Resolve(player, config);
    std::lock_guard lock(mutex);
    if (resolved.result != Result::kReady || state.mode == Mode::kIdle || state.mode == Mode::kHolding ||
        !state.captured)
        return;
    Publish(std::move(resolved));
    state.mode = Mode::kHolding;
    state.fade = 1;
    state.fromStart = false;
    writing.store(true, std::memory_order_release);
    logger::info("[CarryHold] hold after 3D change bones={}", state.bones.size());
}
void Release(bool immediate, float seconds) {
    std::lock_guard lock(mutex);
    if (state.mode != Mode::kHolding && state.mode != Mode::kReleasing) return;
    const float blend = std::isfinite(seconds) && seconds >= 0 ? seconds : state.blendOut;
    if (immediate || !(blend > 0)) return StopWriting("released-immediately");
    // A release already in progress keeps its speed.
    if (state.mode == Mode::kHolding) {
        // Released while still blending in from the last playing sample: release from the pose
        // written now, at the full weight it was written with.
        if (state.fromStart) {
            for (auto& bone : state.bones)
                bone.hold = CarryHoldMath::Blend<RE::NiTransform, RE::NiQuaternion>(bone.start, bone.hold, state.fade);
            state.fade = 1;
            state.fromStart = false;
        }
        state.releaseBlend = blend;
        logger::info("[CarryHold] release blendOut={:.2f} writes={}", blend, state.writes);
    }
    state.mode = Mode::kReleasing;
}
void Reset() {
    std::lock_guard lock(mutex);
    writing.store(false, std::memory_order_release);
    state = {};
}
}
