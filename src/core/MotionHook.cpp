#include "PCH.h"
#include "core/MotionHook.h"
#include "runtime/ActionRegistry.h"
#include "runtime/ActionRunner.h"
#include "runtime/PairFeatures.h"
#include "RE/M/MotionDataContainer.h"
#include <xbyak/xbyak.h>

namespace AnimationInjector::MotionHook {

namespace {
using ChainTransFn = void (*)(std::uintptr_t*, float, RE::NiPoint3&, const RE::BSFixedString*, RE::Character*);
using ChainRotFn = void (*)(std::uintptr_t*, float, RE::NiQuaternion&, const RE::BSFixedString*, RE::Character*);
// One chain per call site. Both sites usually call the same function; when another mod
// hooked only one of them, each site keeps its own previous target.
std::array<ChainTransFn, 2> _chainTrans{};
std::array<ChainRotFn, 2> _chainRot{};
bool _installed = false;

bool Ours(const RE::BSFixedString* a_clip, RE::Character* a_character, int& a_action, int& a_variant) {
    if (!a_clip || !a_clip->c_str() || !a_character) return false;
    return Actions::FindClip(a_clip->c_str(), a_action, a_variant);
}

void ProcessTranslation(RE::MotionDataContainer* a_container, float a_motionTime, RE::NiPoint3& a_translation,
    const RE::BSFixedString* a_clipName, RE::Character* a_character, ChainTransFn a_chain) {
    int action = -1, variant = -1;
    // Motion data is processed on animation worker threads; hold the runner for the read.
    std::shared_ptr<ActionRunner> runner;
    if (Ours(a_clipName, a_character, action, variant)) {
        if (PairFeatures::Translation(a_character->GetHandle(), action, a_motionTime, a_translation)) return;
        runner = Actions::RunnerRef(a_character, action);
        // A single action's clip moves by its HKX: the next in the chain (AMR reads the annotations).
    }
    if (a_chain) a_chain(&a_container->translationDataPtr, a_motionTime, a_translation, a_clipName, a_character);
    if (runner) {
        runner->SteerMotion(a_motionTime, a_translation);
        runner->TraceMotionOutput(a_motionTime, a_translation);
    }
}

void ProcessRotation(RE::MotionDataContainer* a_container, float a_motionTime, RE::NiQuaternion& a_rotation,
    const RE::BSFixedString* a_clipName, RE::Character* a_character, ChainRotFn a_chain) {
    int action = -1, variant = -1;
    if (Ours(a_clipName, a_character, action, variant) &&
        PairFeatures::Rotation(a_character->GetHandle(), action, a_motionTime, a_rotation))
        return;
    if (a_chain)
        a_chain(reinterpret_cast<std::uintptr_t*>(a_container), a_motionTime, a_rotation, a_clipName, a_character);
}
template<int Slot>
void ProcessTranslationAt(RE::MotionDataContainer* a_container, float a_motionTime, RE::NiPoint3& a_translation,
    const RE::BSFixedString* a_clipName, RE::Character* a_character) {
    ProcessTranslation(a_container, a_motionTime, a_translation, a_clipName, a_character, _chainTrans[Slot]);
}
template<int Slot>
void ProcessRotationAt(RE::MotionDataContainer* a_container, float a_motionTime, RE::NiQuaternion& a_rotation,
    const RE::BSFixedString* a_clipName, RE::Character* a_character) {
    ProcessRotation(a_container, a_motionTime, a_rotation, a_clipName, a_character, _chainRot[Slot]);
}
// Target of the rel32 call at a verified call site.
std::uintptr_t CallTarget(std::uintptr_t site) { return site + 5 + *reinterpret_cast<const std::int32_t*>(site + 1); }
}

void Install() {
    if (_installed) return;
    static REL::Relocation<std::uintptr_t> processMotionData{REL::RelocationID(31949, 32703)};
    const bool isAE = REL::Module::IsAE();
    const std::uintptr_t t1 = processMotionData.address() + (isAE ? 0x298 : 0x28D);
    const std::uintptr_t t2 = processMotionData.address() + (isAE ? 0x2AA : 0x2A1);
    const std::uintptr_t r1 = processMotionData.address() + (isAE ? 0x35C : 0x355);
    const std::uintptr_t r2 = processMotionData.address() + (isAE ? 0x36D : 0x368);

    // Each site must still be a rel32 call; otherwise a different game build or another
    // mod's patch occupies it, and writing our call would corrupt the function.
    for (const auto site : {t1, t2, r1, r2}) {
        if (*reinterpret_cast<const std::uint8_t*>(site) != 0xE8) {
            logger::error("[Motion] ProcessMotionData +0x{:X} is not a call; motion hook not installed",
                site - processMotionData.address());
            return;
        }
    }

    // 호출부 레지스터: r13 = Character*, rbx = BSFixedString*(AE 는 rbx-0x14) — AMR/FH_Grapple 과 동일
    struct TransHook : Xbyak::CodeGenerator {
        explicit TransHook(std::uintptr_t fn) {
            Xbyak::Label l;
            sub(rsp, 0x28);
            mov(ptr[rsp + 0x20], r13);
            mov(r9, rbx);
            if (REL::Module::IsAE()) sub(r9, 0x14);
            call(ptr[rip + l]);
            add(rsp, 0x28);
            ret();
            L(l);
            dq(fn);
        }
    };
    struct RotHook : Xbyak::CodeGenerator {
        explicit RotHook(std::uintptr_t fn) {
            Xbyak::Label l;
            sub(rsp, 0x28);
            mov(ptr[rsp + 0x20], r13);
            mov(r9, rbx);
            if (REL::Module::IsAE()) sub(r9, 0x14);
            call(ptr[rip + l]);
            add(rsp, 0x28);
            ret();
            L(l);
            dq(fn);
        }
    };
    // 이전 대상으로 넘길 때 호출부 레지스터 상태를 복원 (AMR 의 썽크도 rbx/r13 을 읽는다)
    struct ChainTrans : Xbyak::CodeGenerator {
        ChainTrans(std::uintptr_t fn, bool ae) {
            Xbyak::Label l;
            push(r13);
            push(rbx);
            sub(rsp, 0x28);
            mov(r13, ptr[rsp + 0x60]);
            if (ae)
                lea(rbx, ptr[r9 + 0x14]);
            else
                mov(rbx, r9);
            call(ptr[rip + l]);
            add(rsp, 0x28);
            pop(rbx);
            pop(r13);
            ret();
            L(l);
            dq(fn);
        }
    };
    struct ChainRot : Xbyak::CodeGenerator {
        ChainRot(std::uintptr_t fn, bool ae) {
            Xbyak::Label l;
            push(r13);
            push(rbx);
            sub(rsp, 0x28);
            mov(r13, ptr[rsp + 0x60]);
            if (ae)
                lea(rbx, ptr[r9 + 0x14]);
            else
                mov(rbx, r9);
            call(ptr[rip + l]);
            add(rsp, 0x28);
            pop(rbx);
            pop(r13);
            ret();
            L(l);
            dq(fn);
        }
    };
    auto& tr = SKSE::GetTrampoline();
    // Read both previous targets before writing. The common case (same target at both sites)
    // installs one thunk and one chain per kind, exactly as before.
    const std::array<std::uintptr_t, 2> prevT{CallTarget(t1), CallTarget(t2)};
    const std::array<std::uintptr_t, 2> prevR{CallTarget(r1), CallTarget(r2)};
    const bool splitT = prevT[0] != prevT[1], splitR = prevR[0] != prevR[1];
    if (splitT || splitR)
        logger::warn(
            "[Motion] call sites chain to different targets (translation={}, rotation={}); each site keeps its own chain",
            splitT, splitR);
    TransHook th0{reinterpret_cast<std::uintptr_t>(&ProcessTranslationAt<0>)},
        th1{reinterpret_cast<std::uintptr_t>(&ProcessTranslationAt<1>)};
    RotHook rh0{reinterpret_cast<std::uintptr_t>(&ProcessRotationAt<0>)},
        rh1{reinterpret_cast<std::uintptr_t>(&ProcessRotationAt<1>)};
    ChainTrans ct0{prevT[0], isAE}, ct1{prevT[1], isAE};
    ChainRot cr0{prevR[0], isAE}, cr1{prevR[1], isAE};
    _chainTrans[0] = reinterpret_cast<ChainTransFn>(tr.allocate(ct0));
    _chainTrans[1] = splitT ? reinterpret_cast<ChainTransFn>(tr.allocate(ct1)) : _chainTrans[0];
    _chainRot[0] = reinterpret_cast<ChainRotFn>(tr.allocate(cr0));
    _chainRot[1] = splitR ? reinterpret_cast<ChainRotFn>(tr.allocate(cr1)) : _chainRot[0];
    void* tThunk0 = tr.allocate(th0);
    void* tThunk1 = splitT ? tr.allocate(th1) : tThunk0;
    void* rThunk0 = tr.allocate(rh0);
    void* rThunk1 = splitR ? tr.allocate(rh1) : rThunk0;
    tr.write_call<5>(t1, tThunk0);
    tr.write_call<5>(t2, tThunk1);
    tr.write_call<5>(r1, rThunk0);
    tr.write_call<5>(r2, rThunk1);
    _installed = true;
    logger::info("[Motion] ProcessMotionData chain installed (AE={}, prevTrans={:p}/{:p})", isAE,
        reinterpret_cast<void*>(prevT[0]), reinterpret_cast<void*>(prevT[1]));
}

}  // namespace AnimationInjector::MotionHook
