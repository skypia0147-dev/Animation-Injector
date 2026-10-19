#include "PCH.h"
#include "core/PairRootRuntime.h"
#include "core/PairRootHandoff.h"
#include "runtime/ActionRegistry.h"
#include "runtime/Carry.h"
#include "runtime/PairFeatures.h"
#include "Settings.h"
#include <array>
#include <atomic>
#include <mutex>
#include <cmath>
#include <cstring>
#include <hde64.h>
namespace AnimationInjector::PairRootRuntime {
namespace {
using Ticket = PairedSession::Ticket;
// Every Animation Injector pair since 0.12.255 ([Compatibility] iPairEndPosition 1 or 2; the per-pair XML
// EndPosition is gone). A pair whose root does not walk away stops at once (no-root-displacement).
bool Enabled() {
    static const bool value = FrameworkSettings::Get().pairEndPosition >= 1;
    return value;
}
struct Clip {
    Ticket ticket{};
    std::uintptr_t characterIdentity = 0, clipIdentity = 0, skeletonIdentity = 0;
    int rootBone = -1, rootParent = -1;
    RE::NiPoint3 rootReference{};
    bool rootReferenceValid = false;
    std::uintptr_t boneArrayIdentity = 0;
    std::array<float, 3> copiedInput{}, copiedBefore{}, copiedAfter{};
    std::uint64_t copiedSequence = 0;
    int copiedRoute = -1;
    RE::NiTransform appliedTransform{};
    std::uint64_t appliedSequence = 0;
    RE::ActorHandle actor;
    float time = -1, duration = -1;
    bool exited = false;
    std::uint64_t sequence = 0;
    std::array<char, 260> file{};
    std::array<char, 80> prefix{};
};
std::mutex ingress;
std::array<Clip, 2> clips{};
std::uint64_t sequence = 0, frame = 0;
std::atomic_bool capturing{false};
struct HookSite {
    std::uintptr_t address = 0;
    std::array<std::byte, 5> installed{};
};
std::array<HookSite, 8> nativeSites{};
Ticket ticket{};
std::array<RE::ActorHandle, 2> actors{};
bool active = false, nativeCopy = false;
float tail = 0;
}
namespace {
template<int Route>
struct ScenePoseCopy {
    using Bones = RE::BSTArray<RE::BShkbAnimationGraph::BoneNodeEntry>;
    static bool ReadLocal(const RE::BShkbAnimationGraph::BoneNodeEntry& entry, std::array<float, 3>& out) {
        if (!entry.node) return false;
        const RE::NiTransform* transform = &entry.node->local;
        const auto index = static_cast<std::int32_t>(entry.unk08);
        if (index >= 0) {
            const auto* tree = netimmerse_cast<RE::BSFlattenedBoneTree*>(entry.node);
            if (!tree) return false;
            const auto& data = tree->GetRuntimeData();
            if (!data.boneEntries || static_cast<std::uint32_t>(index) >= data.numBones) return false;
            const auto& mapped = data.boneEntries[index];
            transform = mapped.node ? &mapped.node->local : &mapped.local;
        }
        const auto& t = transform->translate;
        out = {t.x, t.y, t.z};
        return true;
    }
    static void thunk(const RE::hkQsTransform* pose, Bones* bones, int count) {

        std::array<float, 3> before{};
        int role = -1, bone = -1;

        Ticket capturedTicket{};
        const bool armed = capturing.load(std::memory_order_relaxed);
        if (armed) {

            if (!pose || !bones) {
            } else {
                std::unique_lock lock(ingress, std::try_to_lock);
                if (!lock.owns_lock()) {
                } else
                    for (int i = 0; i < 2; ++i) {
                        const auto& c = clips[i];
                        if (c.boneArrayIdentity != reinterpret_cast<std::uintptr_t>(bones)) continue;
                        const int limit = count < 0 ? static_cast<int>(bones->size())
                                                    : std::min(count, static_cast<int>(bones->size()));
                        if (c.rootBone < 0 || c.rootBone >= limit) {
                            break;
                        }
                        const auto& entry = (*bones)[c.rootBone];
                        if (!ReadLocal(entry, before)) {
                            break;
                        }
                        role = i;
                        bone = c.rootBone;
                        capturedTicket = c.ticket;
                        break;
                    }
            }
        }
        original(pose, bones, count);
        if (role < 0) return;
        if (!capturing.load(std::memory_order_relaxed)) return;
        std::unique_lock lock(ingress, std::try_to_lock);
        if (!lock.owns_lock()) {
            return;
        }
        auto& c = clips[role];
        if (!(c.ticket == capturedTicket) || c.boneArrayIdentity != reinterpret_cast<std::uintptr_t>(bones)) {
            return;
        }
        if (static_cast<std::size_t>(bone) >= bones->size()) {
            return;
        }
        std::array<float, 3> after{};
        if (!ReadLocal((*bones)[bone], after)) {
            return;
        }
        std::memcpy(c.copiedInput.data(), &pose[bone], sizeof(float) * 3);
        c.copiedBefore = before;
        c.copiedAfter = after;
        c.copiedSequence = ++sequence;
        c.copiedRoute = Route;
    }
    static inline REL::Relocation<decltype(thunk)> original;
};
void InstallNativePostPhysicsProbe(int route) {
    // AE Address Library IDs were mapped to loaded 1.6.1170 code. This optional
    // diagnostic skips other editions; it never changes plugin compatibility.
    if (!REL::Module::IsAE()) {
        logger::info("[PairNative] diagnostic skipped: AE call mapping required");
        return;
    }
    constexpr std::array<std::uint64_t, 8> callerIDs{59455, 59846, 37996, 63563, 63566, 63568, 63572, 63589};
    constexpr std::array<std::uint64_t, 8> targetIDs{58293, 58289, 63605, 63856, 63856, 63856, 63856, 63856};
    REL::Relocation<std::uintptr_t> caller{REL::ID(callerIDs[route])}, target{REL::ID(targetIDs[route])};
    const auto start = caller.address();
    DWORD64 imageBase = 0;
    const auto* entry = ::RtlLookupFunctionEntry(start, &imageBase, nullptr);
    const auto& text = REL::Module::get().segment(REL::Segment::textx);
    if (!entry || imageBase + entry->BeginAddress != start) {
        logger::warn("[PairNative] skipped: no exact unwind entry");
        return;
    }
    // x64 shrink-wrapped prologues split this function into chained unwind
    // entries. Extend only through contiguous entries with the same chain root.
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(imageBase);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(imageBase + dos->e_lfanew);
    const auto imageSize = nt->OptionalHeader.SizeOfImage;
    auto sameRoot = [&](const RUNTIME_FUNCTION* candidate) {
        RUNTIME_FUNCTION current = *candidate;
        for (int depth = 0; depth < 32; ++depth) {
            if (current.BeginAddress == entry->BeginAddress) return true;
            const auto offset = current.UnwindData;
            if (offset > imageSize || imageSize - offset < 4) return false;
            const auto* info = reinterpret_cast<const std::uint8_t*>(imageBase + offset);
            if ((info[0] & 7) != 1 || !(info[0] & 0x20)) return false;
            const std::size_t chainedOffset = 4 + ((static_cast<std::size_t>(info[2]) + 1) & ~std::size_t(1)) * 2;
            if (chainedOffset + sizeof(current) > imageSize - offset) return false;
            std::memcpy(&current, info + chainedOffset, sizeof(current));
        }
        return false;
    };
    auto end = imageBase + entry->EndAddress;
    for (int fragments = 0; fragments < 32; ++fragments) {
        DWORD64 nextBase = 0;
        const auto* next = ::RtlLookupFunctionEntry(end, &nextBase, nullptr);
        if (!next || nextBase != imageBase || imageBase + next->BeginAddress != end || !sameRoot(next)) break;
        const auto nextEnd = imageBase + next->EndAddress;
        if (nextEnd <= end || nextEnd - start > 0x10000) return;
        end = nextEnd;
    }
    if (start < text.address() || end > text.address() + text.size() || end <= start || end - start > 0x10000) return;
    std::uintptr_t site = 0;
    unsigned count = 0;
    for (auto at = start; at < end;) {
        hde64s ins{};
        const auto n = hde64_disasm(reinterpret_cast<const void*>(at), &ins);
        if (!n || (ins.flags & F_ERROR) || at + n > end) {
            logger::warn("[PairNative] skipped: decode failed");
            return;
        }
        if (n == 5 && ins.opcode == 0xE8 && at + n + static_cast<std::int32_t>(ins.imm.imm32) == target.address()) {
            site = at;
            ++count;
        }
        at += n;
    }
    if (count != 1) {
        logger::warn("[PairNative] skipped: expected one native call, found {}", count);
        return;
    }
    if (route == 3)
        ScenePoseCopy<3>::original = SKSE::GetTrampoline().write_call<5>(site, ScenePoseCopy<3>::thunk);
    else if (route == 4)
        ScenePoseCopy<4>::original = SKSE::GetTrampoline().write_call<5>(site, ScenePoseCopy<4>::thunk);
    else if (route == 5)
        ScenePoseCopy<5>::original = SKSE::GetTrampoline().write_call<5>(site, ScenePoseCopy<5>::thunk);
    else if (route == 6)
        ScenePoseCopy<6>::original = SKSE::GetTrampoline().write_call<5>(site, ScenePoseCopy<6>::thunk);
    else if (route == 7)
        ScenePoseCopy<7>::original = SKSE::GetTrampoline().write_call<5>(site, ScenePoseCopy<7>::thunk);

    nativeSites[route].address = site;
    std::memcpy(nativeSites[route].installed.data(), reinterpret_cast<const void*>(site), 5);
    logger::info("[PairNative] read-only route={} installed; output is never modified", route);
}
}
void ObserveClip(Ticket value, RE::ActorHandle actor, const RE::hkbContext& context,
    const RE::BSSynchronizedClipGenerator& generator, bool exiting) {
    if (!Enabled()) return;
    auto* clip = generator.clipGenerator;
    if (!clip) return;
    std::lock_guard lock(ingress);
    auto* slot = &clips[0];
    if (clips[0].ticket == value && clips[0].actor != actor) slot = &clips[1];
    if (!(slot->ticket == value) || slot->actor != actor) *slot = {};
    slot->ticket = value;
    slot->actor = actor;
    slot->time = clip->localTime;
    slot->exited = exiting;
    slot->sequence = ++sequence;
    slot->characterIdentity = reinterpret_cast<std::uintptr_t>(context.character);
    slot->clipIdentity = reinterpret_cast<std::uintptr_t>(clip);
    const auto* setup = context.character ? context.character->setup.get() : nullptr;
    const auto* skeleton = setup ? setup->animationSkeleton.get() : nullptr;
    if (skeleton && slot->skeletonIdentity != reinterpret_cast<std::uintptr_t>(skeleton)) {
        slot->skeletonIdentity = reinterpret_cast<std::uintptr_t>(skeleton);
        slot->rootBone = -1;
        slot->rootParent = -1;
        slot->rootReference = {};
        slot->rootReferenceValid = false;
        static const RE::BSFixedString rootName("NPC Root [Root]");
        if (skeleton->bones.size() > 0 && skeleton->bones.size() < 4096)
            for (int i = 0; i < skeleton->bones.size(); ++i) {
                const char* name = skeleton->bones[i].name.c_str();
                if (name && std::strcmp(name, rootName.c_str()) == 0) {
                    slot->rootBone = i;
                    if (i < skeleton->parentIndices.size()) slot->rootParent = skeleton->parentIndices[i];
                    if (i < skeleton->referencePose.size()) {
                        const auto& v = skeleton->referencePose[i].translation.quad;
                        slot->rootReference = {v.m128_f32[0], v.m128_f32[1], v.m128_f32[2]};
                        slot->rootReferenceValid = std::isfinite(v.m128_f32[0]) && std::isfinite(v.m128_f32[1]) &&
                                                   std::isfinite(v.m128_f32[2]);
                    }
                    break;
                }
            }
    }
    auto* control = clip->animationControl.get();
    if (control && control->binding && control->binding->animation)
        slot->duration = control->binding->animation->duration;
    const char* file = clip->animationName.c_str();
    const char* prefix = generator.syncAnimPrefix.c_str();
    if (file) {
        const auto n = std::min(std::strlen(file), slot->file.size() - 1);
        std::memcpy(slot->file.data(), file, n);
        slot->file[n] = 0;
    }
    if (prefix) {
        const auto n = std::min(std::strlen(prefix), slot->prefix.size() - 1);
        std::memcpy(slot->prefix.data(), prefix, n);
        slot->prefix[n] = 0;
    }
}

void Reset() {
    PairRootHandoff::Reset();
    capturing.store(false);
    active = false;
    ticket = {};
    actors = {};
    tail = 0;
    std::lock_guard lock(ingress);
    clips = {};
    sequence = 0;
    frame = 0;
}
bool Busy(Ticket value) { return active && value == ticket && PairRootHandoff::Busy(value); }
void Install() {
    if (!Enabled()) return;
    if (REL::Module::IsAE())
        for (int route = 3; route < 8; ++route) InstallNativePostPhysicsProbe(route);
    nativeCopy = std::any_of(nativeSites.begin() + 3, nativeSites.end(), [](const auto& s) { return s.address != 0; });
    logger::info("[PairRootRuntime] observation={} (scene sampling fallback requires in-game validation)",
        nativeCopy ? "native-copy" : "main-thread-scene");
}
void Sample(float delta, bool afterRuntime) {
    if (!Enabled() || !std::isfinite(delta) || delta <= 0) return;
    const auto status = PairedSession::ReadStatus();
    const bool live = status.phase != PairedSession::Phase::kIdle;
    if (!(ticket == status.ticket)) {
        PairRootHandoff::Reset();
        ticket = status.ticket;
        tail = 0;
        active = live;
        actors = {status.initiator.actor, status.target.actor};
        capturing.store(active);
    }
    if (!active) return;
    if (!afterRuntime) {
        ++frame;
        tail = live ? 0 : tail + delta;
    }
    std::array<Clip, 2> meta;
    {
        std::lock_guard lock(ingress);
        meta = clips;
    }
    static const RE::BSFixedString rootName("NPC Root [Root]");
    for (int role = 0; role < 2; ++role) {
        // Carry preserves a full root transform, including vertical movement.
        // Do not also rebase the carried actor's controller during that exit.
        if (Carry::OwnsTargetRoot(ticket, actors[role])) {
            if (afterRuntime) PairRootHandoff::Cancel(role);
            continue;
        }
        auto actor = actors[role].get();
        if (!actor || !actor->Is3DLoaded()) {
            if (afterRuntime) PairRootHandoff::Cancel(role);
            continue;
        }
        auto* scene = actor->Get3D(false);
        if (!scene) {
            if (afterRuntime) PairRootHandoff::Cancel(role);
            continue;
        }
        Clip sample{};
        for (const auto& c : meta)
            if (c.ticket == ticket && c.actor == actors[role]) sample = c;
        RE::BSTSmartPointer<RE::BSAnimationGraphManager> manager;
        if (actor->GetAnimationGraphManager(manager) && manager) {
            RE::BSSpinLockGuard graphLock(manager->GetRuntimeData().updateLock);
            for (const auto& graph : manager->graphs) {
                if (!graph || reinterpret_cast<std::uintptr_t>(&graph->characterInstance) != sample.characterIdentity)
                    continue;
                std::unique_lock lock(ingress, std::try_to_lock);
                if (lock.owns_lock())
                    for (auto& c : clips)
                        if (c.ticket == ticket && c.actor == actors[role])
                            c.boneArrayIdentity = reinterpret_cast<std::uintptr_t>(&graph->boneNodes);
                break;
            }
        }
        if (afterRuntime) {
            // Never borrow a different participant's skeleton reference pose.
            if (!sample.rootReferenceValid) continue;
            const auto& participant = role == 0 ? status.initiator : status.target;
            const bool exit =
                participant.endEventSeen || sample.exited || status.phase == PairedSession::Phase::kStopping || !live;
            const bool allow = !status.rootHandoffSuppressed &&
                               (status.lastEnd == PairedSession::EndReason::kNone ||
                                   status.lastEnd == PairedSession::EndReason::kCompleted ||
                                   status.lastEnd == PairedSession::EndReason::kParticipantUnavailable ||
                                   status.lastEnd == PairedSession::EndReason::kCancelled ||
                                   status.lastEnd == PairedSession::EndReason::kMovementRecovery);
            PairRootHandoff::Update(ticket, role, actor.get(), scene->GetObjectByName(rootName), sample.file.data(),
                sample.time, sample.duration, delta, allow, nativeCopy ? sample.copiedSequence : frame, exit,
                status.rootExitBudget, sample.rootReference,
                status.lastEnd == PairedSession::EndReason::kMovementRecovery,
                PairFeatures::MovementRecoveryActive(ticket, actors[role]), live && participant.inheritedAir);
        }
    }
    if (afterRuntime && tail > status.rootExitBudget + 0.25f) {
        active = false;
        capturing.store(false);
    }
}
}
