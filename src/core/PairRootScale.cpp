#include "PCH.h"
#include "core/PairRootScale.h"
#include "core/GraphBoneSlot.h"
#include "core/PoseTrackScale.h"
#include "runtime/PairedSession.h"
#include <array>
#include <atomic>
#include <cmath>

namespace AnimationInjector::PairRootScale {
namespace {
constexpr int poseTrack = 2;  // hkbGeneratorOutput::TRACK_POSE
// Main thread only.
struct Participant {
    bool known = false;  // this handle + 3D were examined (compensated or not)
    RE::ActorHandle handle;
    RE::FormID form = 0;
    RE::NiPointer<RE::NiAVObject> scene;
    bool scaled = false;
};
std::array<Participant, 2> table;
// Read by the generate hook on animation threads. The character address is an identity only.
std::array<std::atomic<std::uintptr_t>, 2> characters{};
std::array<std::atomic<float>, 2> inverse{};
std::array<std::atomic<std::uint32_t>, 2> writes{}, rejected{};
std::atomic_bool active{false};

struct Resolved {
    std::uintptr_t character = 0;
    float parentScale = 0;
    RE::BSFixedString root;
};
// Main thread. The animation hook never takes a lock, so the graph lock cannot nest with ours.
bool Resolve(RE::Actor* actor, Resolved& out) {
    RE::BSTSmartPointer<RE::BSAnimationGraphManager> manager;
    if (!actor->GetAnimationGraphManager(manager) || !manager) return false;
    RE::BSSpinLockGuard lock(manager->GetRuntimeData().updateLock);
    const auto graph = manager->GetRuntimeData().activeGraph;
    if (graph >= manager->graphs.size() || !manager->graphs[graph]) return false;
    auto& current = *manager->graphs[graph];
    const auto& bones = current.boneNodes;
    if (bones.empty() || !bones[0].node) return false;
    // Graph bone 0 (pose track bone 0) is the animation skeleton's root, where paired travel lives.
    const auto entry = static_cast<std::int32_t>(bones[0].unk08);
    const auto slot = GraphBoneSlot::Access(bones[0].node, entry);
    if (!slot.parent) return false;
    out.character = reinterpret_cast<std::uintptr_t>(&current.characterInstance);
    out.parentScale = slot.parent->scale;
    out.root = GraphBoneSlot::Name(bones[0].node, entry);
    return true;
}
void Release(unsigned i, const char* reason) {
    auto& p = table[i];
    characters[i].store(0, std::memory_order_release);
    if (p.scaled)
        logger::info("[PairRootScale] released actor={:08X} reason={} writes={} rejected={}", p.form, reason,
            writes[i].exchange(0), rejected[i].exchange(0));
    p = {};
    active.store(characters[0].load() || characters[1].load(), std::memory_order_release);
}
// Every clip's output for a registered character: the pair clip, and whatever it blends with
// on entry and exit (their root travel is zero or scales the same way).
struct ClipGenerate {
    static void thunk(RE::hkbClipGenerator* clip, const RE::hkbContext& context,
        const PoseTrackReader::Output** children, PoseTrackReader::Output& output, float offset) {
        original(clip, context, children, output, offset);
        if (!active.load(std::memory_order_acquire) || !context.character) return;
        const auto id = reinterpret_cast<std::uintptr_t>(context.character);
        for (unsigned i = 0; i < 2; ++i)
            if (characters[i].load(std::memory_order_acquire) == id) {
                const auto result =
                    PoseTrackScale::ScaleTranslation(output, poseTrack, 0, inverse[i].load(std::memory_order_relaxed));
                ++(result == 0 ? writes[i] : rejected[i]);
                return;
            }
    }
    static inline REL::Relocation<decltype(thunk)> original;
};
}
void Install() {
    REL::Relocation<std::uintptr_t> clip{RE::VTABLE_hkbClipGenerator[0]};
    ClipGenerate::original = clip.write_vfunc(0x17, ClipGenerate::thunk);
    logger::info("[PairRootScale] installed at hkbClipGenerator::Generate exit");
}
void Update() {
    const auto pair = PairedSession::ReadStatus();
    const bool running = pair.phase != PairedSession::Phase::kIdle;
    const std::array<RE::ActorHandle, 2> want{
        running ? pair.initiator.actor : RE::ActorHandle{}, running ? pair.target.actor : RE::ActorHandle{}};
    for (unsigned i = 0; i < 2; ++i) {
        auto actor = want[i].get();
        auto* scene = actor ? actor->Get3D(false) : nullptr;
        auto& p = table[i];
        if (p.known && p.handle == want[i] && p.scene.get() == scene && scene) continue;
        Release(i, running ? "participant-changed" : "pair-ended");
        if (!actor || !scene) continue;
        p.known = true;
        p.handle = want[i];
        p.form = actor->GetFormID();
        p.scene.reset(scene);
        const float scale = actor->GetScale();
        if (!std::isfinite(scale) || std::abs(scale - 1) < 0.001f) continue;
        Resolved r;
        if (!Resolve(actor.get(), r) || !std::isfinite(r.parentScale) || r.parentScale < 0.01f) {
            logger::info(
                "[PairRootScale] actor={:08X} scale={:.3f}: root bone unavailable; not compensated", p.form, scale);
            continue;
        }
        p.scaled = true;
        inverse[i].store(1.0f / r.parentScale, std::memory_order_relaxed);
        characters[i].store(r.character, std::memory_order_release);
        active.store(true, std::memory_order_release);
        logger::info("[PairRootScale] actor={:08X} scale={:.3f} parentScale={:.3f} root='{}'", p.form, scale,
            r.parentScale, r.root.c_str() ? r.root.c_str() : "");
    }
}
void Reset() {
    for (unsigned i = 0; i < 2; ++i) Release(i, "reset");
}
}
