#pragma once
#include "PCH.h"

// Havok 2010 구조 중 CommonLib 에 없거나 unk 로만 있는 것. 전부 런타임 자가검증(GraphInjector (a)(b)(c))을 거친 배치다.
namespace RE {
using hkStateMachine_TransitionInfoArray_t = hkbStateMachine::TransitionInfoArray;
}

namespace AnimationInjector::Havok {

struct TimeInterval {
    std::int32_t enterEventId;  // 00  -1 = 없음(구간 시작 = 상태 진입)
    std::int32_t exitEventId;   // 04
    float enterTime;            // 08  ★시간 기반 구간은 이 게임에서 채택되지 않는다 — 이벤트 기반만 쓸 것 (V9 실측)
    float exitTime;             // 0C
};
static_assert(sizeof(TimeInterval) == 0x10);

struct TransitionInfo {
    // hkbStateMachine::TransitionInfo::TransitionFlags 실제 값 (Nemesis 숫자형 3072 = LOCAL|GLOBAL 로 교차 확인)
    enum Flags : std::int16_t {
        kUseTriggerInterval = 0x1,
        kUseInitiateInterval = 0x2,
        kUninterruptibleWhilePlaying = 0x4,
        kUninterruptibleWhileDelayed = 0x8,
        kDelayStateChange = 0x10,
        kDisabled = 0x20,
        kDisallowReturnToPreviousState = 0x40,
        kDisallowRandomTransition = 0x80,
        kDisableCondition = 0x100,
        kAllowSelfTransitionByTransitionFromAnyState = 0x200,
        kIsGlobalWildcard = 0x400,
        kIsLocalWildcard = 0x800,
        kFromNestedStateIdIsValid = 0x1000,
        kToNestedStateIdIsValid = 0x2000,
        kAbutAtEndOfFromGenerator = 0x4000,
    };
    TimeInterval triggerInterval;        // 00
    TimeInterval initiateInterval;       // 10
    RE::hkReferencedObject* transition;  // 20  hkbTransitionEffect*
    RE::hkReferencedObject* condition;   // 28  hkbCondition*
    std::int32_t eventId;                // 30
    std::int32_t toStateId;              // 34
    std::int32_t fromNestedStateId;      // 38
    std::int32_t toNestedStateId;        // 3C
    std::int16_t priority;               // 40  ★진행 중 전이보다 낮으면 끼어들기도 보관도 안 된다
    std::int16_t flags;                  // 42
    std::uint32_t pad44;                 // 44
};
static_assert(sizeof(TransitionInfo) == 0x48);

// hkbStateMachine::StateInfo (CommonLib 은 unk30~unk70). 0x78
struct StateInfoLayout {
    std::uint8_t bindable[0x30];                            // hkbBindable
    RE::hkArray<RE::hkRefVariant> listeners;                // 30
    RE::hkReferencedObject* enterNotifyEvents;              // 40  hkbStateMachineEventPropertyArray*
    RE::hkReferencedObject* exitNotifyEvents;               // 48
    RE::hkStateMachine_TransitionInfoArray_t* transitions;  // 50
    RE::hkbGenerator* generator;                            // 58
    RE::hkStringPtr name;                                   // 60
    std::int32_t stateId;                                   // 68
    float probability;                                      // 6C
    bool enable;                                            // 70
    std::uint8_t pad71[7];                                  // 71
};
static_assert(sizeof(StateInfoLayout) == 0x78);

struct TransitionInfoArrayLayout {  // hkbStateMachine::TransitionInfoArray 0x20
    std::uint8_t refObj[0x10];
    RE::hkArray<TransitionInfo> transitions;  // 10
};
static_assert(sizeof(TransitionInfoArrayLayout) == 0x20);

}  // namespace AnimationInjector::Havok
