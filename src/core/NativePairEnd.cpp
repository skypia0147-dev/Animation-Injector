#include "PCH.h"
#include "core/NativePairEnd.h"
#include "core/NativePairEndPlan.h"
#include "core/PairRootHandoff.h"
#include "Settings.h"
#include <array>
#include <cstring>
#include <mutex>

namespace AnimationInjector::NativePairEnd {
namespace {
namespace Plan = NativePairEndPlan;
// One participant as the graph threads last reported it. Values only: the actor is a handle, the skeleton an
// identity that is compared, never dereferenced here.
struct Observation {
    RE::ActorHandle actor;
    std::uint64_t owner = 0;     // one participation; the actor's next pair gets a new one
    std::uint64_t observed = 0;  // counts reports, so the main thread sees a pair that stopped reporting
    float time = -1, duration = -1;
    bool looping = false, deactivated = false, excluded = false;
    RE::NiPoint3 reference{};  // the root bone's rest translation (skeleton reference pose)
    bool referenceValid = false;
    std::uintptr_t skeleton = 0;
    std::array<char, 128> file{};
};
std::mutex ingress;  // graph threads write, the main thread copies; no engine call is made while it is held
Plan::Slots slots;
std::array<Observation, Plan::kCapacity> observations;
std::uint64_t owners = 0;
bool fullReported = false;
// Main thread only.
struct Handoff {
    std::uint64_t owner = 0, lastObserved = 0;
    float unseen = 0;  // seconds since the graph last reported the participant
    bool announced = false;
    PairRootHandoff::State state;
};
std::array<Handoff, Plan::kCapacity> handoffs;
std::uint64_t frame = 0;
bool installed = false;

void FindRoot(Observation& o, const RE::hkaSkeleton* skeleton) {
    o.skeleton = reinterpret_cast<std::uintptr_t>(skeleton);
    o.reference = {};
    o.referenceValid = false;
    static const RE::BSFixedString rootName("NPC Root [Root]");
    if (skeleton->bones.size() <= 0 || skeleton->bones.size() >= 4096) return;
    for (int i = 0; i < skeleton->bones.size(); ++i) {
        const char* name = skeleton->bones[i].name.c_str();
        if (!name || std::strcmp(name, rootName.c_str()) != 0) continue;
        if (i < skeleton->referencePose.size()) {
            const auto& v = skeleton->referencePose[i].translation.quad;
            o.reference = {v.m128_f32[0], v.m128_f32[1], v.m128_f32[2]};
            o.referenceValid =
                std::isfinite(o.reference.x) && std::isfinite(o.reference.y) && std::isfinite(o.reference.z);
        }
        return;
    }
}
// Main thread: forget a participant, unless the graph already started its next pair in that slot.
void Release(int slot, std::uint32_t id, std::uint64_t owner) {
    std::lock_guard lock(ingress);
    if (slots.identity[static_cast<std::size_t>(slot)] != id ||
        observations[static_cast<std::size_t>(slot)].owner != owner)
        return;
    slots.Release(slot);
    observations[static_cast<std::size_t>(slot)] = {};
}
}

Scope Configured() {
    switch (FrameworkSettings::Get().pairEndPosition) {
    case 0: return Scope::kOff;
    case 1: return Scope::kAiPairs;
    default: return Scope::kEvery;
    }
}
void Install() {
    installed = Configured() == Scope::kEvery;
    logger::info("[PairEnd] end position for pairs Animation Injector did not start: {} (iPairEndPosition={})",
        installed ? "on" : "off", FrameworkSettings::Get().pairEndPosition);
}
void ObserveClip(
    RE::Actor* actor, const RE::hkbContext& context, const RE::BSSynchronizedClipGenerator& sync, bool exiting) {
    if (!installed || !actor) return;
    auto* clip = sync.clipGenerator;
    auto* character = context.character;
    if (!clip || !character) return;
    // The player's first-person graph plays the same scene; only the third-person skeleton places the actor.
    const auto* setup = character->setup.get();
    const auto* data = setup ? setup->data.get() : nullptr;
    const auto* strings = data ? data->stringData.get() : nullptr;
    const char* project = strings ? strings->name.c_str() : nullptr;
    if (project && _stricmp(project, "FirstPerson") == 0) return;
    const auto handle = actor->GetHandle();
    const auto id = handle.native_handle();
    std::lock_guard lock(ingress);
    int slot = slots.Find(id);
    if (slot < 0) {
        // A clip ending that was never seen playing (it started before a load): nothing to hand over.
        if (exiting) return;
        slot = slots.Claim(id);
        if (slot < 0) {
            if (!fullReported) {
                fullReported = true;
                logger::warn("[PairEnd] {} pairs end at once; actor={:08X} keeps the engine's own end", Plan::kCapacity,
                    actor->GetFormID());
            }
            return;
        }
        observations[static_cast<std::size_t>(slot)] = {};
    }
    auto& o = observations[static_cast<std::size_t>(slot)];
    if (!o.owner || (o.deactivated && !exiting)) {
        // A new pair, or the actor's next one before the main thread released the last.
        o = {};
        o.owner = ++owners;
    }
    o.actor = handle;
    ++o.observed;
    o.time = clip->localTime;
    o.looping = clip->mode.get() != RE::hkbClipGenerator::PlaybackMode::kModeSinglePlay;
    if (exiting) o.deactivated = true;
    const auto* control = clip->animationControl.get();
    if (control && control->binding && control->binding->animation) o.duration = control->binding->animation->duration;
    if (const char* name = clip->animationName.c_str(); name && !o.file[0]) {
        const auto n = std::min(std::strlen(name), o.file.size() - 1);
        std::memcpy(o.file.data(), name, n);
        o.file[n] = 0;
        o.excluded = Plan::Excluded(name);
    }
    if (const auto* skeleton = setup ? setup->animationSkeleton.get() : nullptr;
        skeleton && o.skeleton != reinterpret_cast<std::uintptr_t>(skeleton))
        FindRoot(o, skeleton);
}
void Update(float delta) {
    if (!installed || !std::isfinite(delta) || delta <= 0) return;
    ++frame;
    std::array<Observation, Plan::kCapacity> seen;
    std::array<std::uint32_t, Plan::kCapacity> ids{};
    {
        std::lock_guard lock(ingress);
        seen = observations;
        ids = slots.identity;
    }
    static const RE::BSFixedString rootName("NPC Root [Root]");
    static const RE::BSFixedString syncedName("bIsSynced");
    for (std::size_t i = 0; i < Plan::kCapacity; ++i) {
        auto& h = handoffs[i];
        const auto& o = seen[i];
        if (!ids[i] || !o.owner) {
            h = {};
            continue;
        }
        if (h.owner != o.owner) {
            h = {};
            h.owner = o.owner;
            h.lastObserved = o.observed;
        }
        if (o.observed != h.lastObserved) {
            h.lastObserved = o.observed;
            h.unseen = 0;
        } else
            h.unseen += delta;
        const auto actor = o.actor.get();
        bool release = !actor || h.unseen > Plan::kForget;
        if (!release && !h.state.stopped && !o.excluded && o.referenceValid) {
            if (actor->IsOnMount()) {
                h.state.owner = o.owner;
                PairRootHandoff::Stop(h.state, "mounted");
            } else {
                bool synced = true;  // unchanged when the graph has no such variable
                actor->GetGraphVariableBool(syncedName, synced);
                const bool ending = Plan::EndBegins(o.time, o.duration, o.looping, o.deactivated, synced);
                if (ending && !h.announced) {
                    h.announced = true;
                    logger::info(
                        "[PairEnd] #{} actor={:08X} '{}' end begins clip={:.2f}/{:.2f} deactivated={} synced={}",
                        o.owner, actor->GetFormID(), o.file.data(), o.time, o.duration, o.deactivated, synced);
                }
                auto* scene = actor->Get3D(false);
                // Movement after the pair (the player's input, an NPC's next package) is accepted: the handoff
                // rebases only the disappearing root offset onto wherever the actor is now. The scene freezes the
                // controller, which reports "in air" for a frame near the end (traced 2026-10-03: every kill move,
                // 0.3-0.4 s before its end); that is not a fall, as for the AI pairs' inherited air.
                PairRootHandoff::Update(h.state, o.owner, 0,
                    PairRootHandoff::Frame{actor.get(), scene ? scene->GetObjectByName(rootName) : nullptr, o.reference,
                        delta, true, frame, ending, Plan::kBudget, false, true, true});
            }
        }
        // A finished handoff keeps its slot until the pair is over, so the same pair never starts again.
        const bool done = h.state.stopped || o.excluded || !o.referenceValid;
        if (done && (o.deactivated || h.unseen > 0.5f)) release = true;
        if (release) {
            Release(static_cast<int>(i), ids[i], o.owner);
            h = {};
        }
    }
}
void Reset() {
    std::lock_guard lock(ingress);
    slots = {};
    observations = {};
    handoffs = {};
    fullReported = false;
}
}
