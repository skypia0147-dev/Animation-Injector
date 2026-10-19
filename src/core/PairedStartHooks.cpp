#include "PCH.h"
#include "core/PairedStartHooks.h"
#include "core/PairedClipBinding.h"
#include "core/AnimationPreloader.h"
#include "core/NativePairAlignment.h"
#include "core/SpaceProbe.h"
#include "core/PairPlacementMath.h"
#include "core/NodeFactory.h"
#include "core/AnimationBindingWithTriggers.h"
#include "Settings.h"
#include <hde64.h>

namespace AnimationInjector::PairedStartHooks {
namespace {
thread_local std::uint64_t requestId = 0;
thread_local RE::ActorHandle initiatorHandle, targetHandle;
thread_local std::optional<PairPreflight::Placement> preflightPlacement;
bool oarLoaded = false;
struct ClipView {
    bool slot = false, readable = false, loaded = false, prefixTrack = false;
    bool baseBinding = false, syncBinding = false, syncSlot = false, syncLoaded = false;
    bool controlBinding = false;
    bool syncSlotChecked = false;
    int syncIndex = -1, markTrack = -1, transformTracks = -1;
    bool prefixNull = false, prefixManaged = false;
    int tracks = -1;
    float duration = 0.0f;
};
// The engine's wrapper can have no Havok class metadata. Use the verified
// typed member only after the Address Library vtable matches.
RE::hkaAnimationBinding* ReadBinding(RE::hkbAnimationBindingWithTriggers* slot, bool& readable) {
    if (!NodeFactory::SameVTable(slot, RE::VTABLE_hkbAnimationBindingWithTriggers)) return nullptr;
    readable = true;
    return slot->binding.get();
}
ClipView Inspect(RE::BSSynchronizedClipGenerator* gen, const RE::hkbContext& ctx) {
    ClipView view;
    auto* clip = gen ? gen->clipGenerator : nullptr;
    auto* ch = ctx.character;
    if (!clip || !ch) return view;
    std::uintptr_t prefixBits = 0;
    static_assert(sizeof(prefixBits) == sizeof(gen->syncAnimPrefix));
    std::memcpy(&prefixBits, &gen->syncAnimPrefix, sizeof(prefixBits));
    view.prefixNull = prefixBits == 0;
    view.prefixManaged = (prefixBits & RE::hkStringPtr::kManaged) != 0;
    auto* set = static_cast<RE::hkbAnimationBindingSet*>(ch->animationBindingSet.get());
    if (!set && ch->setup) set = ch->setup->animationBindingSet.get();
    const int index = clip->animationBindingIndex;
    RE::hkaAnimationBinding* binding = nullptr;
    if (set && index >= 0 && index < set->bindings._size && set->bindings._data) {
        auto* slot = set->bindings._data[index];
        view.slot = slot != nullptr;
        if (slot) binding = ReadBinding(slot, view.readable);
    }
    view.baseBinding = binding != nullptr;
    view.syncBinding = gen->binding != nullptr;
    view.syncIndex = gen->synchronizedAnimationBindingIndex;
    // OAR stores a relative sync index and restores its project-specific
    // offset at activation. Do not interpret it as an absolute slot here.
    if (!oarLoaded && set && view.syncIndex >= 0 && view.syncIndex < set->bindings._size && set->bindings._data) {
        view.syncSlotChecked = true;
        auto* slot = set->bindings._data[view.syncIndex];
        bool readable = false;
        auto* sync = ReadBinding(slot, readable);
        view.syncSlot = slot != nullptr;
        view.syncLoaded = sync && sync->animation.get();
    }
    auto* control = clip->animationControl.get();
    if (control && control->binding) {
        binding = control->binding;
        view.readable = true;
        view.controlBinding = true;
    }
    auto* anim = binding ? binding->animation.get() : nullptr;
    if (!anim) return view;
    view.loaded = true;
    view.duration = anim->duration;
    view.tracks = anim->annotationTracks._size;
    view.transformTracks = anim->numberOfTransformTracks;
    const char* prefix = gen->syncAnimPrefix.c_str();
    if (view.tracks < 0 || view.tracks > 4096 || (view.tracks && !anim->annotationTracks._data)) return view;
    for (int i = 0; i < view.tracks; ++i) {
        const char* name = anim->annotationTracks._data[i].trackName.c_str();
        // Native GetMark substitutes "NPC" only for a null prefix.
        if (name && std::strcmp(name, prefix ? prefix : "NPC") == 0) {
            view.prefixTrack = true;
            view.markTrack = i;
        }
    }
    return view;
}
void LogView(const ClipView& view, const char* phase) {
    logger::debug(
        "[PairStart] #{} binding {} basePresent={} controlPresent={} syncPresent={} syncIndex={} syncSlot={} syncLoaded={} readable={} animation={} duration={:.3f} transformTracks={} annotationTracks={} markTrack={} prefixNull={} prefixManaged={} syncSlotChecked={}",
        requestId, phase, view.baseBinding, view.controlBinding, view.syncBinding, view.syncIndex, view.syncSlot,
        view.syncLoaded, view.readable, view.loaded, view.duration, view.transformTracks, view.tracks, view.markTrack,
        view.prefixNull, view.prefixManaged, view.syncSlotChecked);
}
struct MarkTrace {
    RE::BSSynchronizedClipGenerator* gen;
    RE::hkbCharacter* character;
    int queues = 0, queueResult = -999, loads = 0, unloads = 0;
    bool loadResult = false;
    ClipView afterQueue;
};
// Borrowed only across the current native GetMark stack frame.
thread_local MarkTrace* currentMark = nullptr;
struct MarkScope {
    MarkTrace* previous;
    explicit MarkScope(MarkTrace& mark) : previous(currentMark) { currentMark = &mark; }
    ~MarkScope() { currentMark = previous; }
};
bool Matches(RE::BSSynchronizedClipGenerator* gen, const RE::hkbContext& ctx) {
    return currentMark && currentMark->gen == gen && currentMark->character == ctx.character;
}
struct QueueHook {
    static std::int32_t thunk(RE::AnimationFileManagerSingleton* self, const RE::hkbContext& ctx,
        RE::hkbClipGenerator* clip, RE::BSSynchronizedClipGenerator* gen) {
        const auto result = original(self, ctx, clip, gen);
        if (Matches(gen, ctx)) {
            ++currentMark->queues;
            currentMark->queueResult = result;
            currentMark->afterQueue = Inspect(gen, ctx);
        }
        return result;
    }
    static inline REL::Relocation<decltype(thunk)> original;
};
struct LoadHook {
    static bool thunk(RE::AnimationFileManagerSingleton* self, const RE::hkbContext& ctx, RE::hkbClipGenerator* clip,
        RE::BSSynchronizedClipGenerator* gen) {
        const bool result = original(self, ctx, clip, gen);
        if (Matches(gen, ctx)) {
            ++currentMark->loads;
            currentMark->loadResult = result;
        }
        return result;
    }
    static inline REL::Relocation<decltype(thunk)> original;
};
struct UnloadHook {
    static std::int32_t thunk(RE::AnimationFileManagerSingleton* self, const RE::hkbContext& ctx,
        RE::hkbClipGenerator* clip, RE::BSSynchronizedClipGenerator* gen) {
        if (Matches(gen, ctx)) ++currentMark->unloads;
        return original(self, ctx, clip, gen);
    }
    static inline REL::Relocation<decltype(thunk)> original;
};
// Signatures follow preserved native callers. GetMark also matches the local
// OAR Offsets.h declaration BSSynchronizedClipGenerator_UnkActivate.
using HasFn = bool (*)(void*, const RE::ObjectRefHandle&);
using AddFn = bool (*)(RE::BGSSynchronizedAnimationInstance*, const RE::ObjectRefHandle&, const RE::BSFixedString&);
using StartFn = bool (*)(RE::BGSSynchronizedAnimationInstance*);
using MarkFn = bool (*)(RE::BSSynchronizedClipGenerator*, const RE::hkbContext&, RE::hkQsTransform&);
using MoveFn = bool (*)(RE::bhkCharacterController*, const RE::hkVector4&, void*, bool);
HasFn origHas = nullptr;
AddFn origAdd = nullptr;
StartFn origStart = nullptr;
MarkFn origMark = nullptr;
MoveFn origMove = nullptr;
struct StartTrace {
    int marks = 0, marksOK = 0, moves = 0, movesRejected = 0;
    std::array<PairPlacementMath::Mark, 2> placementMarks{};
    bool placementMarksValid = true;
    std::array<RE::NiTransform, 2> actorTransforms{};
    bool actorTransformsValid = false;
};
thread_local StartTrace* currentStart = nullptr;
// Borrowed only within StartHook's stack frame, including nested starts.
thread_local NativePairAlignment::Start* currentAlignment = nullptr;
struct StartScope {
    StartTrace* previous;
    NativePairAlignment::Start* previousAlignment;
    StartScope(StartTrace& trace, NativePairAlignment::Start& alignment)
        : previous(currentStart), previousAlignment(currentAlignment) {
        currentStart = &trace;
        currentAlignment = &alignment;
    }
    ~StartScope() {
        currentStart = previous;
        currentAlignment = previousAlignment;
    }
};
bool MoveHook(RE::bhkCharacterController* controller, const RE::hkVector4& position, void* args, bool ignoreDepth) {
    const RE::hkVector4 requested = position;
    RE::hkVector4 placed = position;
    // Every scene (vanilla kill moves included, so not gated on a traced request): the victim keeps its
    // position and the attacker's placement goes to its pivot goal, still checked by the engine there.
    if (currentStart && currentAlignment) {
        using Move = NativePairAlignment::Start::Move;
        const auto move = currentAlignment->NativeMove(requestId, controller, placed);
        if (move == Move::kSkipped || move == Move::kBlocked) {
            ++currentStart->moves;
            if (move == Move::kBlocked) ++currentStart->movesRejected;
            logger::debug("[PairStart] #{} native placement {}", requestId,
                move == Move::kSkipped ? "skipped: victim keeps its position" : "rejected, attacker path blocked");
            return move == Move::kSkipped;
        }
        if (move == Move::kRedirected)
            logger::debug("[PairStart] #{} native placement redirected to the pivot goal", requestId);
    }
    if (requestId && currentStart && FrameworkSettings::Get().spaceProbe) {
        const auto& trace = *currentStart;
        auto self = initiatorHandle.get(), target = targetHandle.get();
        const auto relative =
            trace.marks == 2 && trace.placementMarksValid
                ? PairPlacementMath::RelativePosition(trace.placementMarks[0], trace.placementMarks[1])
                : std::nullopt;
        int anchor = -1;
        if (self && target && self->Is3DLoaded() && target->Is3DLoaded()) {
            const bool moveSelf = self->GetCharController() == controller;
            const bool moveTarget = target->GetCharController() == controller;
            if (moveSelf != moveTarget) anchor = moveSelf ? 1 : 0;
        }
        if (relative && anchor >= 0 && trace.actorTransformsValid && trace.moves == 0) {
            const auto& world = trace.actorTransforms[anchor];
            // Native Start uses rotation and translation, without actor scale.
            const RE::NiPoint3 local{(*relative)[0], (*relative)[1], (*relative)[2]};
            const auto predicted = world.translate + world.rotate * local;
            const auto& raw = requested.quad.m128_f32;
            const float error = (predicted - RE::NiPoint3{raw[0], raw[1], raw[2]}).Length();
            logger::debug(
                "[PairPlacementCheck] #{} anchorRole={} predicted=({:.3f},{:.3f},{:.3f}) native=({:.3f},{:.3f},{:.3f}) error={:.6f} match={} diagnosticOnly=true",
                requestId, anchor, predicted.x, predicted.y, predicted.z, raw[0], raw[1], raw[2], error,
                std::isfinite(error) && error <= 0.1f);
            if (preflightPlacement) {
                // The preflight predicts the pivot placement, so compare it with where the engine is sent.
                const auto& pre = preflightPlacement->positions[1 - anchor];
                const auto& sent = placed.quad.m128_f32;
                const float preError = (pre - RE::NiPoint3{sent[0], sent[1], sent[2]}).Length();
                logger::debug(
                    "[PairPreflightCheck] #{} beforeSetup=true error={:.6f} match={} predicted=({:.3f},{:.3f},{:.3f}) diagnosticOnly=true",
                    requestId, preError, std::isfinite(preError) && preError <= 0.1f, pre.x, pre.y, pre.z);
            }
        } else
            logger::debug("[PairPlacementCheck] #{} unavailable marks={} anchor={} diagnosticOnly=true", requestId,
                trace.marks, anchor);
    }
    if (requestId && currentStart && !preflightPlacement)
        SpaceProbe::BeforePlacement(requestId, initiatorHandle, targetHandle, controller, placed);
    const bool result = origMove(controller, placed, args, ignoreDepth);
    if (requestId && currentStart) {
        ++currentStart->moves;
        if (!result) ++currentStart->movesRejected;
        logger::debug("[PairStart] #{} native placement TryMoveTo result={}", requestId, result);
        const auto& xyz = requested.quad.m128_f32;
        const auto& sent = placed.quad.m128_f32;
        logger::debug(
            "[PairPlacement] #{} native-request-raw=({:.2f},{:.2f},{:.2f}) sent=({:.2f},{:.2f},{:.2f}) ignoreDepth={} result={}",
            requestId, xyz[0], xyz[1], xyz[2], sent[0], sent[1], sent[2], ignoreDepth, result);
    }
    return result;
}
bool HasHook(void* manager, const RE::ObjectRefHandle& actor) {
    const bool result = origHas(manager, actor);
    if (requestId)
        logger::debug("[PairStart] #{} HasActor handle={:08X} result={}", requestId, actor.native_handle(), result);
    return result;
}
bool AddHook(
    RE::BGSSynchronizedAnimationInstance* scene, const RE::ObjectRefHandle& actor, const RE::BSFixedString& event) {
    const bool result = origAdd(scene, actor, event);
    if (requestId)
        logger::debug("[PairStart] #{} AddActor handle={:08X} event='{}' result={}", requestId, actor.native_handle(),
            event.c_str(), result);
    return result;
}
bool StartHook(RE::BGSSynchronizedAnimationInstance* scene) {
    StartTrace trace;
    NativePairAlignment::Start alignment(scene);
    StartScope scope(trace, alignment);
    if (requestId && FrameworkSettings::Get().spaceProbe) {
        auto self = initiatorHandle.get(), target = targetHandle.get();
        if (self && target && self.get() != target.get() && self->Is3DLoaded() && target->Is3DLoaded()) {
            self->GetTransform(trace.actorTransforms[0]);
            target->GetTransform(trace.actorTransforms[1]);
            trace.actorTransformsValid = true;
        }
    }
    const bool result = origStart(scene);
    alignment.Complete(result);
    if (requestId)
        logger::debug("[PairStart] #{} Start result={} marks(ok/calls)={}/{} placement(calls/rejected)={}/{}",
            requestId, result, trace.marksOK, trace.marks, trace.moves, trace.movesRejected);
    return result;
}
bool MarkHook(RE::BSSynchronizedClipGenerator* gen, const RE::hkbContext& ctx, RE::hkQsTransform& output) {
    PairedClipBinding::MarkScope binding(gen, ctx);
    if (!requestId) {
        const bool result = origMark(gen, ctx, output);
        if (!result) AnimationPreloader::RecoverFailedMark(ctx, gen ? gen->clipGenerator : nullptr);
        if (currentAlignment) currentAlignment->RecordNativeMark(gen, ctx.character, result, output);
        return result;
    }
    const auto before = Inspect(gen, ctx);
    LogView(before, "before-GetMark");
    MarkTrace mark{gen, ctx.character};
    MarkScope scope(mark);
    auto* clip = gen ? gen->clipGenerator : nullptr;
    logger::debug(
        "[PairStart] #{} GetMark begin node='{}' character={:X} clipIndex={} userData={} control={} slot={} bindingReadable={} loaded={}",
        requestId, gen ? gen->name.c_str() : "(none)", reinterpret_cast<std::uintptr_t>(ctx.character),
        clip ? static_cast<int>(static_cast<std::int16_t>(clip->animationBindingIndex)) : -1, clip ? clip->userData : 0,
        clip && clip->animationControl.get(), before.slot, before.readable, before.loaded);
    const bool result = origMark(gen, ctx, output);
    if (!result) AnimationPreloader::RecoverFailedMark(ctx, gen ? gen->clipGenerator : nullptr);
    if (currentAlignment) currentAlignment->RecordNativeMark(gen, ctx.character, result, output);
    if (currentStart && FrameworkSettings::Get().spaceProbe) {
        const int index = currentStart->marks;
        if (!result || index >= 2)
            currentStart->placementMarksValid = false;
        else {
            auto& value = currentStart->placementMarks[index];
            for (int i = 0; i < 3; ++i) value.translation[i] = output.translation.quad.m128_f32[i];
            for (int i = 0; i < 4; ++i) value.rotation[i] = output.rotation.vec.quad.m128_f32[i];
            logger::debug("[PairPlacementCheck] #{} mark={} leader={} applyRoot={} q=({:.6f},{:.6f},{:.6f},{:.6f})",
                requestId, index, gen && gen->leadCharacter, gen && gen->applyMotionFromRoot, value.rotation[0],
                value.rotation[1], value.rotation[2], value.rotation[3]);
        }
    }
    if (result) {
        const auto& xyz = output.translation.quad.m128_f32;
        logger::debug("[PairPlacement] #{} mark node='{}' prefix='{}' translationRaw=({:.4f},{:.4f},{:.4f})", requestId,
            gen ? gen->name.c_str() : "", gen && gen->syncAnimPrefix.c_str() ? gen->syncAnimPrefix.c_str() : "NPC",
            xyz[0], xyz[1], xyz[2]);
    }
    if (currentStart) {
        ++currentStart->marks;
        if (result) ++currentStart->marksOK;
    }
    logger::debug(
        "[PairStart] #{} GetMark end node='{}' result={} Queue(calls/result)={}/{} Load(calls/result)={}/{} UnloadCalls={}",
        requestId, gen ? gen->name.c_str() : "(none)", result, mark.queues, mark.queueResult, mark.loads,
        mark.loadResult, mark.unloads);
    const auto& view = mark.queues ? mark.afterQueue : before;
    LogView(view, mark.queues ? "after-Queue" : "no-Queue");
    logger::debug(
        "[PairStart] #{} GetMark animation slot={} bindingReadable={} loaded={} duration={:.3f} tracks={} prefix='{}' prefixTrack={}",
        requestId, view.slot, view.readable, view.loaded, view.duration, view.tracks,
        gen && gen->syncAnimPrefix.c_str() ? gen->syncAnimPrefix.c_str() : "", view.prefixTrack);
    return result;
}
std::vector<std::uintptr_t> FindCalls(REL::RelocationID parent, REL::RelocationID target) {
    const auto address = parent.address();
    DWORD64 imageBase = 0;
    const auto* entry = ::RtlLookupFunctionEntry(address, &imageBase, nullptr);
    const auto& text = REL::Module::get().segment(REL::Segment::textx);
    if (!entry || imageBase + entry->BeginAddress != address) return {};
    const auto end = imageBase + entry->EndAddress;
    if (address < text.address() || end > text.address() + text.size() || end <= address || end - address > 0x10000)
        return {};
    std::vector<std::uintptr_t> sites;
    // Decode instruction boundaries within the PE unwind range. Embedded E8
    // bytes in operands must never be mistaken for native call sites.
    for (auto at = address; at < end;) {
        hde64s ins{};
        const auto length = hde64_disasm(reinterpret_cast<const void*>(at), &ins);
        if (!length || (ins.flags & F_ERROR) || at + length > end) return {};
        if (length == 5 && ins.opcode == 0xE8 &&
            at + length + static_cast<std::int32_t>(ins.imm.imm32) == target.address())
            sites.push_back(at);
        at += length;
    }
    return sites;
}
}
Attempt::Attempt(std::uint64_t request, RE::ActorHandle initiator, RE::ActorHandle target, int action)
    : previous(requestId), previousInitiator(initiatorHandle), previousTarget(targetHandle),
      previousPlacement(preflightPlacement) {
    requestId = (FrameworkSettings::Get().debug || FrameworkSettings::Get().spaceProbe) ? request : 0;
    initiatorHandle = initiator;
    targetHandle = target;
    preflightPlacement.reset();
    if (FrameworkSettings::Get().spaceProbe && action >= 0) {
        preflightPlacement = PairPreflight::Read(initiator, target, action);
        logger::debug(
            "[PairPreflightCheck] #{} resident-read available={} action={} beforeSetup=true diagnosticOnly=true",
            requestId, preflightPlacement.has_value(), action);
        if (preflightPlacement) {
            auto actor = initiator.get();
            if (actor && actor->Is3DLoaded()) {
                const auto& p = preflightPlacement->positions[0];
                SpaceProbe::BeforePlacement(
                    requestId, initiator, target, actor->GetCharController(), RE::hkVector4(p.x, p.y, p.z, 0));
            }
        }
    }
}
Attempt::~Attempt() {
    requestId = previous;
    initiatorHandle = previousInitiator;
    targetHandle = previousTarget;
    preflightPlacement = previousPlacement;
}
void Install() {
    if (REL::Module::IsVR()) {
        logger::warn(
            "[NativePairAlign] native call-site alignment is not installed on VR; original placement retained");
        return;
    }
    oarLoaded = REX::W32::GetModuleHandleW(L"OpenAnimationReplacer.dll") != nullptr;
    const auto has = FindCalls(REL::RelocationID{32049, 32803}, REL::RelocationID{32054, 32808});
    const auto add = FindCalls(REL::RelocationID{32049, 32803}, REL::RelocationID{32029, 32782});
    const auto start = FindCalls(REL::RelocationID{32049, 32803}, REL::RelocationID{32039, 32792});
    // The gameplay hook must not depend on optional diagnostic call sites.
    if (start.size() != 1) {
        logger::error("[NativePairAlign] common native Start hook unavailable: validated sites={}", start.size());
        return;
    }
    auto& tr = SKSE::GetTrampoline();
    origStart = reinterpret_cast<StartFn>(tr.write_call<5>(start[0], StartHook));
    NativePairAlignment::InstallActivationHook();
    logger::info("[NativePairAlign] common native Start hook installed (automatic paired/killmove alignment)");
    // Native marks are gameplay input, not optional debug instrumentation.
    const auto mark = FindCalls(REL::RelocationID{32039, 32792}, REL::RelocationID{62122, 63058});
    if (mark.size() == 2) {
        for (auto site : mark) origMark = reinterpret_cast<MarkFn>(tr.write_call<5>(site, MarkHook));
        logger::info("[NativePairAlign] native mark capture installed (no extra Queue/Load)");
    } else
        logger::warn("[NativePairAlign] native mark capture unavailable: sites={}; resident-only alignment retained",
            mark.size());
    // The attacker's placement is gameplay too (until 0.12.255 it was hooked only with bDebug/bSpaceProbe,
    // so a default INI never kept the chosen role still). Without it Prepare places after native Start.
    const auto move = FindCalls(REL::RelocationID{32039, 32792}, REL::RelocationID{76421, 78260});
    if (move.size() == 1 && mark.size() == 2) {
        origMove = reinterpret_cast<MoveFn>(tr.write_call<5>(move[0], MoveHook));
        logger::info("[NativePairAlign] native placement redirect installed (victim keeps its position)");
    } else
        logger::warn("[NativePairAlign] native placement redirect unavailable: TryMoveTo sites={} marks={}",
            move.size(), mark.size());
    if (!FrameworkSettings::Get().debug && !FrameworkSettings::Get().spaceProbe) return;
    if (has.size() != 2 || add.size() != 2 || start.size() != 1 || mark.size() != 2 || move.size() != 1) {
        logger::warn(
            "[PairStart] probes skipped: validated calls HasActor={} AddActor={} Start={} GetMark={} TryMoveTo={}",
            has.size(), add.size(), start.size(), mark.size(), move.size());
        return;
    }
    for (auto site : has) origHas = reinterpret_cast<HasFn>(tr.write_call<5>(site, HasHook));
    for (auto site : add) origAdd = reinterpret_cast<AddFn>(tr.write_call<5>(site, AddHook));
    REL::Relocation<std::uintptr_t> fm{RE::VTABLE_AnimationFileManagerSingleton[0]};
    QueueHook::original = fm.write_vfunc(1, QueueHook::thunk);
    LoadHook::original = fm.write_vfunc(2, LoadHook::thunk);
    UnloadHook::original = fm.write_vfunc(3, UnloadHook::thunk);
    logger::debug(
        "[PairStart] native probes installed: HasActor=2 AddActor=2 Start=1 GetMark=2 TryMoveTo=1, file manager Queue/Load/Unload");
}
}
