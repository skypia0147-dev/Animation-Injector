#pragma once
#include "PCH.h"
#include "core/HavokLayout.h"
#include "core/NameIndex.h"

// Havok 노드를 런타임에 만든다 — 0 으로 채운 버퍼 + Address Library vtable + 필드 직접 세팅.
//   memSizeAndFlags 0xFFFF / referenceCount 0x4000 → Havok 이 절대 해제하지 않는다. hkArray 는 kDontDealloc.
//   클론(액터별)은 반사로 정상 복제된다 (DirectDodge 실측).
namespace AnimationInjector::NodeFactory {

// BSIsActiveModifier has no GetClassType metadata. Its native virtual clone owns
// object sizing, field copying and reference acquisition; only templates qualify.
RE::hkbNode* CloneActivityModifier(RE::hkbNode* source, const char* name);

constexpr std::uint32_t kDontDealloc = static_cast<std::uint32_t>(1) << 31;

void* AllocZero(std::size_t a_size);
void InitRefObj(void* a_obj, const std::array<REL::VariantID, 1>& a_vtable);
void MakeString(RE::hkStringPtr& a_dst, const char* a_text);
void AddRef(RE::hkReferencedObject* a_obj);
bool SameVTable(const void* a_obj, const std::array<REL::VariantID, 1>& a_vtable);
const char* NodeName(const void* a_node);

template<class T>
void EmptyArray(RE::hkArray<T>& a) {
    a._data = nullptr;
    a._size = 0;
    a._capacityAndFlags = kDontDealloc;
}
template<class T>
void OwnArray(RE::hkArray<T>& a, T* a_data, std::int32_t a_n) {
    a._data = a_data;
    a._size = a_n;
    a._capacityAndFlags = static_cast<std::int32_t>(a_n | kDontDealloc);
}

// 이름 배열 (eventNames / animationNames)
int FindName(RE::hkArray<RE::hkStringPtr>& a_names, const char* a_name);
bool AppendName(RE::hkArray<RE::hkStringPtr>& a_names, const char* a_name);

// Local to a synchronous registration phase. Destroy before forwarding to the
// engine/other hooks; nested scopes restore their caller's lookup context.
class NameLookupScope {
public:
    explicit NameLookupScope(RE::hkArray<RE::hkStringPtr>& names);
    ~NameLookupScope();
    NameLookupScope(const NameLookupScope&) = delete;
    NameLookupScope& operator=(const NameLookupScope&) = delete;
    static NameLookupScope* For(RE::hkArray<RE::hkStringPtr>& names);
    int Find(const char* name);
    void Appended(const char* name);

private:
    RE::hkArray<RE::hkStringPtr>& names;
    NameTableLookup index;
    NameLookupScope* previous = nullptr;
    static thread_local NameLookupScope* current;
};

// An animation file's index in a project's animationNames, or -1 (first match ignoring case). For
// lookups outside an injection pass, on any thread: one index per table, kept while the table's
// buffer and size stay the same (a table with thousands of names was scanned per call).
int FindAnimationName(RE::hkArray<RE::hkStringPtr>& names, const char* path);
// A project's animation bindings are indexed with 16 bits: 32,767 at most, shared with every other
// animation mod (OAR stops the game above it). Synchronized clips add bindings after the names, so
// this module stops adding names 1,024 below the limit.
inline constexpr int kAnimationTableLimit = 0x7FFF - 1024;
// True (and logged once per table) when no more animation names may be added.
bool AnimationTableFull(const RE::hkArray<RE::hkStringPtr>& names, const char* project);
// 이벤트 등록: eventNames + eventInfos(4바이트) 쌍. 이미 있으면 그 id. 실패 -1
int InjectEvent(RE::hkbBehaviorGraph* a_graph, const char* a_name);
const char* EventNameOf(RE::hkbBehaviorGraph* a_graph, std::int32_t a_id);  // 모르면 "?"

// 캐릭터 문자열 데이터의 animationNames 에 a_path 를 보장하고 인덱스를 돌려준다 (없으면 push). 실패 -1
int EnsureAnimationName(RE::hkbCharacter* a_character, const char* a_path);

// 클립 트리거: { 시각, 이벤트 id, relativeToEnd }
struct TriggerDef {
    float time;
    std::int32_t eventId;
    bool relativeToEnd;
};
RE::hkbClipTriggerArray* MakeTriggerArray(const std::vector<TriggerDef>& a_defs);
RE::hkbClipGenerator* MakeClip(const char* a_name, const char* a_animPath, std::uint16_t a_bindingIndex,
    RE::hkbClipTriggerArray* a_triggers, const RE::hkbNode* a_template, bool a_convertAnnotations);
// 페어: 클립을 감싸는 BSSynchronizedClipGenerator (바닐라 HugA: lead=true prefix "2_" / pa_HugA: lead=false prefix "")
RE::hkbGenerator* MakeSyncClip(const char* a_name, RE::hkbClipGenerator* a_clip, const char* a_prefix, bool a_lead,
    float a_getToMarkTime, float a_markErrorThreshold, bool a_reorientSupport, bool a_applyMotionFromRoot,
    const RE::hkbNode* a_template, const RE::hkStringPtr* a_nativePrefix = nullptr);
// Native paired prefixes preserve the reference graph's storage and null value.
// a_nativePrefix is borrowed only during construction; its wrapper is copied.
Havok::TransitionInfoArrayLayout* MakeTransitionArray(std::int32_t a_n);
void FillTransition(Havok::TransitionInfo& t, std::int32_t a_eventId, std::int32_t a_toStateId,
    RE::hkReferencedObject* a_effect, std::int16_t a_flags, std::int16_t a_priority = 0);
Havok::StateInfoLayout* MakeState(const char* a_name, std::int32_t a_stateId, RE::hkbGenerator* a_gen,
    Havok::TransitionInfoArrayLayout* a_transitions);

// 상태머신 안의 기존 전이 효과에서 진입/퇴장용을 고른다 (이름 선호도: …ToAnimationDriven / …FromAnimationDriven)
struct Effects {
    RE::hkReferencedObject* enter = nullptr;
    RE::hkReferencedObject* exit = nullptr;
    std::string enterName, exitName;
};
Effects FindEffects(RE::hkbStateMachine* a_sm);

// hkbBlendingTransitionEffect (SSE 실측, 0x90): hkbNode 0x48 | selfTransitionMode i8 @48 | eventMode i8 @49 | duration f @50 |
//   toGeneratorStartTimeFraction f @54 | flags u16 @58 | endMode i8 @5A | blendCurve i8 @5B | (런타임) fromGenerator @60 toGenerator @68 …
//   기존 효과를 복제(memcpy)한 뒤 이름·길이·eventMode 를 바꾼다. 로드 시점엔 런타임 필드가 비어 있어 복제가 안전하다.
// 배치 자가검증: a_template 에서 읽은 duration 이 0~5 초, 모드 값이 0~3 이어야 한다. 아니면 nullptr (기존 효과 재사용).
RE::hkReferencedObject* MakeBlendEffect(
    const RE::hkReferencedObject* a_template, const char* a_name, float a_duration, int a_eventMode);

}  // namespace AnimationInjector::NodeFactory
