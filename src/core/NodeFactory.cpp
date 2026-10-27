#include "PCH.h"
#include "core/NodeFactory.h"
#include "core/TemplateNodeClone.h"

namespace AnimationInjector::NodeFactory {

RE::hkbNode* CloneActivityModifier(RE::hkbNode* source, const char* name) {
    if (!SameVTable(source, RE::VTABLE_BSIsActiveModifier) || source->cloneState == RE::hkbNode::CloneState::kClone)
        return nullptr;
    // hkbNode slot 0C is CloneNode (CommonLib currently labels it Unk_0C).
    // Verified BSIsActiveModifier implementation: source in RCX, new node in RAX.
    // The Address Library vtable check above restricts this adapter to that type.
    auto* node = TemplateNodeClone::Detached(source,
        [](RE::hkbNode* original) { return REL::RelocateVirtual<RE::hkbNode*(RE::hkbNode*)>(0x0C, 0x0C, original); });
    if (!node) return nullptr;
    if (!SameVTable(node, RE::VTABLE_BSIsActiveModifier) ||
        node->variableBindingSet.get() != source->variableBindingSet.get() || node->cachedBindables._size != 0 ||
        node->areBindablesCached) {
        logger::error("[PairRig] native activity clone did not produce an independent template");
        node->RemoveReference();
        return nullptr;
    }
    // Match the lifetime of all injected graph templates. Retain native allocation
    // metadata; a guessed modifier size is never used for allocation or copying.
    node->referenceCount = 0x4000;
    MakeString(node->name, name);
    return node;
}

void* AllocZero(std::size_t a_size) {
    void* p = _aligned_malloc(a_size, 16);
    if (p) std::memset(p, 0, a_size);
    return p;
}

void InitRefObj(void* a_obj, const std::array<REL::VariantID, 1>& a_vtable) {
    REL::Relocation<std::uintptr_t> vt{a_vtable[0]};
    *reinterpret_cast<std::uintptr_t*>(a_obj) = vt.address();
    auto* ro = reinterpret_cast<RE::hkReferencedObject*>(a_obj);
    ro->memSizeAndFlags = 0xFFFF;  // Havok 이 할당한 객체가 아니다 — 절대 해제하지 않게
    ro->referenceCount = 0x4000;
}

void MakeString(RE::hkStringPtr& a_dst, const char* a_text) {
    using HkStrCtorFn = void (*)(RE::hkStringPtr*, const char*);
    static REL::Relocation<HkStrCtorFn> ctor{REL::RelocationID(56801, 57231)};
    std::memset(&a_dst, 0, sizeof(a_dst));
    ctor(&a_dst, a_text);
}

void AddRef(RE::hkReferencedObject* a_obj) {
    if (a_obj) ::InterlockedIncrement16(reinterpret_cast<volatile SHORT*>(&a_obj->referenceCount));
}

bool SameVTable(const void* a_obj, const std::array<REL::VariantID, 1>& a_vtable) {
    REL::Relocation<std::uintptr_t> vt{a_vtable[0]};
    return a_obj && *reinterpret_cast<const std::uintptr_t*>(a_obj) == vt.address();
}

const char* NodeName(const void* a_node) {
    if (!a_node) return "";
    const char* s = reinterpret_cast<const RE::hkbNode*>(a_node)->name.c_str();
    return s ? s : "";
}

namespace {
// hkbBindable + hkbNode 공통 필드
void InitNodeBase(void* a_obj, const char* a_name, const RE::hkbNode* a_template) {
    auto* b = reinterpret_cast<RE::hkbBindable*>(a_obj);
    std::memset(&b->variableBindingSet, 0, sizeof(b->variableBindingSet));
    EmptyArray(b->cachedBindables);
    b->areBindablesCached = false;
    auto* n = reinterpret_cast<RE::hkbNode*>(a_obj);
    n->userData = 0;
    MakeString(n->name, a_name);
    n->id = a_template ? a_template->id : 0xFFFF;
    n->cloneState = a_template ? a_template->cloneState : RE::hkbNode::CloneState(0);
}
struct HkbEventInfo {
    std::uint32_t flags;
};  // ★NG 헤더는 8바이트로 오표기 — 실제 4바이트 (FH_Grapple 실측)
static_assert(sizeof(HkbEventInfo) == 4);
}

thread_local NameLookupScope* NameLookupScope::current = nullptr;
NameLookupScope::NameLookupScope(RE::hkArray<RE::hkStringPtr>& value) : names(value), previous(current) {
    current = this;
}
NameLookupScope::~NameLookupScope() { current = previous; }
NameLookupScope* NameLookupScope::For(RE::hkArray<RE::hkStringPtr>& value) {
    for (auto* scope = current; scope; scope = scope->previous)
        if (&scope->names == &value) return scope;
    return nullptr;
}
int NameLookupScope::Find(const char* name) {
    return index.Find(names._data, names._size, [&](int i) { return names._data[i].c_str(); }, name);
}
void NameLookupScope::Appended(const char* name) { index.Appended(names._data, names._size, name); }

int FindName(RE::hkArray<RE::hkStringPtr>& a_names, const char* a_name) {
    if (auto* scope = NameLookupScope::For(a_names)) return scope->Find(a_name);
    if (!a_name || !a_names._data) return -1;
    for (int i = 0; i < a_names._size; ++i) {
        const char* s = a_names._data[i].c_str();
        if (s && _stricmp(s, a_name) == 0) return i;
    }
    return -1;
}

bool AppendName(RE::hkArray<RE::hkStringPtr>& a_names, const char* a_name) {
    // Grow by half: thousands of declared names were each copying the whole table every 8 appends.
    if (a_names._size >= a_names.capacity()) a_names.reserve(a_names._size + std::max(8, a_names._size / 2));
    if (a_names._size >= a_names.capacity()) return false;
    MakeString(a_names._data[a_names._size], a_name);
    a_names._size++;
    if (auto* scope = NameLookupScope::For(a_names)) scope->Appended(a_name);
    return true;
}

int InjectEvent(RE::hkbBehaviorGraph* a_graph, const char* a_name) {
    auto* data = a_graph ? a_graph->data.get() : nullptr;
    if (!data || !data->stringData.get()) return -1;
    auto& names = data->stringData->eventNames;
    if (!names._data) return -1;
    const int existing = FindName(names, a_name);
    if (existing >= 0) return existing;
    auto& infos = *reinterpret_cast<RE::hkArray<HkbEventInfo>*>(&data->eventInfos);
    if (static_cast<int>(infos.size()) != names._size) {
        logger::warn("[Node] eventInfos({}) != eventNames({}) — '{}' 건너뜀", infos.size(), names._size, a_name);
        return -1;
    }
    infos.push_back(HkbEventInfo{0});
    if (!AppendName(names, a_name)) {
        // Back to one info per name: left one longer, every later event of this graph was refused
        // (2026-10-28 review R-25).
        infos._size--;
        return -1;
    }
    return names._size - 1;
}

const char* EventNameOf(RE::hkbBehaviorGraph* a_graph, std::int32_t a_id) {
    auto* data = a_graph ? a_graph->data.get() : nullptr;
    if (!data || !data->stringData.get()) return "?";
    auto& names = data->stringData->eventNames;
    if (a_id < 0 || a_id >= names._size) return "?";
    const char* s = names._data[a_id].c_str();
    return s ? s : "?";
}

bool AnimationTableFull(const RE::hkArray<RE::hkStringPtr>& a_names, const char* a_project) {
    if (a_names._size < kAnimationTableLimit) return false;
    static std::mutex lock;
    static std::unordered_set<const void*> reported;
    std::lock_guard guard(lock);
    if (reported.insert(&a_names).second)
        logger::error("[Inject] project '{}' animation table reached {} names (the 32,767 bindings a project can hold, "
                      "shared with other animation mods such as OAR, minus room for synchronized clips); further clips "
                      "are not registered and the actions using them are skipped",
            a_project ? a_project : "?", a_names._size);
    return true;
}

int EnsureAnimationName(RE::hkbCharacter* a_character, const char* a_path) {
    auto* setup = a_character ? a_character->setup.get() : nullptr;
    auto* cdata = setup ? setup->data.get() : nullptr;
    auto* csd = cdata ? cdata->stringData.get() : nullptr;
    if (!csd || !a_path) return -1;
    auto& names = csd->animationNames;
    // During an injection pass the table is indexed (first match; a path is listed once unless the
    // native table itself repeats it, and both entries then bind the same file).
    const char* project = csd->name.c_str();
    if (NameLookupScope::For(names)) {
        if (const int found = FindName(names, a_path); found >= 0) return found;
        if (AnimationTableFull(names, project)) return -1;
        return AppendName(names, a_path) ? names._size - 1 : -1;
    }
    // Clip activation asks on every activation of an injected clip: the cached index, same first-match
    // rule as above.
    if (const int found = FindAnimationName(names, a_path); found >= 0) return found;
    if (AnimationTableFull(names, project) || !AppendName(names, a_path)) return -1;
    return names._size - 1;
}

int FindAnimationName(RE::hkArray<RE::hkStringPtr>& a_names, const char* a_path) {
    if (!a_path || !a_names._data) return -1;
    if (NameLookupScope::For(a_names)) return FindName(a_names, a_path);
    static std::mutex lock;
    static std::unordered_map<const void*, NameTableLookup> tables;
    const auto read = [&](int i) { return a_names._data[i].c_str(); };
    std::lock_guard guard(lock);
    // Keyed by the table's address, which a project loaded later can reuse: the entries only grow as
    // creature and NPC projects come and go, so the cache starts over past a bound (each index is
    // rebuilt on its next query; code review 2026-09-30 R-21).
    if (tables.size() >= 256 && !tables.contains(&a_names)) tables.clear();
    auto& lookup = tables[&a_names];
    int found = lookup.Find(a_names._data, a_names._size, read, a_path);
    // A table rewritten in place (same buffer and size) would leave the index stale: confirm the hit
    // and rebuild once when it does not match.
    if (found >= 0 && (!read(found) || _stricmp(read(found), a_path) != 0)) {
        lookup = NameTableLookup{};
        found = lookup.Find(a_names._data, a_names._size, read, a_path);
    }
    return found;
}

RE::hkbClipTriggerArray* MakeTriggerArray(const std::vector<TriggerDef>& a_defs) {
    auto* arr = static_cast<RE::hkbClipTriggerArray*>(AllocZero(sizeof(RE::hkbClipTriggerArray)));
    InitRefObj(arr, RE::VTABLE_hkbClipTriggerArray);
    const int n = static_cast<int>(a_defs.size());
    auto* t = static_cast<RE::hkClipTrigger*>(AllocZero(sizeof(RE::hkClipTrigger) * std::max(n, 1)));
    for (int i = 0; i < n; ++i) {
        t[i].localTime = a_defs[i].relativeToEnd ? -std::fabs(a_defs[i].time) : a_defs[i].time;
        t[i].event.id = static_cast<RE::hkbEventBase::SystemEventIDs>(a_defs[i].eventId);
        t[i].relativeToEndOfClip = a_defs[i].relativeToEnd;
        t[i].acyclic = false;
        t[i].isAnnotation = false;
    }
    OwnArray(arr->triggers, t, n);
    return arr;
}

RE::hkbClipGenerator* MakeClip(const char* a_name, const char* a_animPath, std::uint16_t a_bindingIndex,
    RE::hkbClipTriggerArray* a_triggers, const RE::hkbNode* a_template, bool a_convertAnnotations) {
    auto* c = static_cast<RE::hkbClipGenerator*>(AllocZero(sizeof(RE::hkbClipGenerator)));
    InitRefObj(c, RE::VTABLE_hkbClipGenerator);
    InitNodeBase(c, a_name, a_template);
    MakeString(c->animationName, a_animPath);
    *reinterpret_cast<RE::hkbClipTriggerArray**>(&c->triggers) = a_triggers;
    c->cropStartAmountLocalTime = 0.0f;
    c->cropEndAmountLocalTime = 0.0f;
    c->startTime = 0.0f;
    c->playbackSpeed = 1.0f;
    c->enforcedDuration = 0.0f;
    c->userControlledTimeFraction = 0.0f;
    c->animationBindingIndex = a_bindingIndex;
    c->mode = RE::hkbClipGenerator::kModeSinglePlay;
    c->flags =
        a_convertAnnotations
            ? 0
            : 16;  // 16 = FLAG_DONT_CONVERT_ANNOTATIONS_TO_TRIGGERS. 슬롯 모델은 주석이 이벤트가 되어야 한다 (V2)
    EmptyArray(c->animDatas);
    EmptyArray(c->echos);
    const float identity[12] = {0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1};
    std::memcpy(&c->extractedMotion, identity, sizeof(identity));
    return c;
}

RE::hkbGenerator* MakeSyncClip(const char* a_name, RE::hkbClipGenerator* a_clip, const char* a_prefix, bool a_lead,
    float a_getToMarkTime, float a_markErrorThreshold, bool a_reorientSupport, bool a_applyMotionFromRoot,
    const RE::hkbNode* a_template, const RE::hkStringPtr* a_nativePrefix) {
    auto* g = static_cast<RE::BSSynchronizedClipGenerator*>(AllocZero(0x130));  // 마지막 멤버 0x12C (헤더 실측)
    InitRefObj(g, RE::VTABLE_BSSynchronizedClipGenerator);
    InitNodeBase(g, a_name, a_template);
    g->clipGenerator = a_clip;
    AddRef(a_clip);
    // Native GetMark reads this field as a raw character pointer: unlike
    // annotation track names, it does not mask hkStringPtr's managed bit.
    // A null native support prefix also means "NPC", not an empty string.
    // The reference and new nodes live in the same owning behavior graph.
    static_assert(std::is_trivially_copy_assignable_v<RE::hkStringPtr>);
    if (a_nativePrefix)
        g->syncAnimPrefix = *a_nativePrefix;
    else {
        // GetMark reads a raw pointer, so a managed hkStringPtr is unsafe here.
        // New species have no native prefix to borrow. Keep an aligned, immutable
        // string in the same process-lifetime allocation pool as our graph nodes.
        const char* prefix = nullptr;
        if (a_prefix && *a_prefix) {
            auto* storage = static_cast<char*>(AllocZero(std::strlen(a_prefix) + 1));
            if (!storage) return nullptr;
            std::memcpy(storage, a_prefix, std::strlen(a_prefix) + 1);
            prefix = storage;
        }
        std::memcpy(&g->syncAnimPrefix, &prefix, sizeof(prefix));
    }
    g->syncClipIgnoreMarkPlacement = false;
    g->getToMarkTime = a_getToMarkTime;
    g->markErrorThreshold = a_markErrorThreshold;
    g->leadCharacter = a_lead;
    g->reorientSupportChar = a_reorientSupport;
    g->applyMotionFromRoot = a_applyMotionFromRoot;
    g->synchronizedScene = nullptr;
    g->synchronizedAnimationBindingIndex = 0xFFFF;
    g->binding = nullptr;
    g->eventWithPrefixIdToEventWithoutPrefixIdMap = nullptr;
    const float identity[12] = {0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1};
    std::memcpy(&g->startingWorldFromModel, identity, sizeof(identity));
    std::memcpy(&g->worldFromModelWithRootMotion, identity, sizeof(identity));
    std::memcpy(&g->unkTransform, identity, sizeof(identity));
    return g;
}

Havok::TransitionInfoArrayLayout* MakeTransitionArray(std::int32_t a_n) {
    auto* arr =
        static_cast<Havok::TransitionInfoArrayLayout*>(AllocZero(sizeof(RE::hkbStateMachine::TransitionInfoArray)));
    InitRefObj(arr, RE::VTABLE_hkbStateMachine__TransitionInfoArray);
    auto* items = static_cast<Havok::TransitionInfo*>(AllocZero(sizeof(Havok::TransitionInfo) * std::max(a_n, 1)));
    OwnArray(arr->transitions, items, a_n);
    return arr;
}

void FillTransition(Havok::TransitionInfo& t, std::int32_t a_eventId, std::int32_t a_toStateId,
    RE::hkReferencedObject* a_effect, std::int16_t a_flags, std::int16_t a_priority) {
    t.triggerInterval = {-1, -1, 0.0f, 0.0f};
    t.initiateInterval = {-1, -1, 0.0f, 0.0f};
    t.transition = a_effect;
    AddRef(a_effect);
    t.condition = nullptr;
    t.eventId = a_eventId;
    t.toStateId = a_toStateId;
    t.fromNestedStateId = 0;
    t.toNestedStateId = 0;
    t.priority = a_priority;
    t.flags = a_flags;
    t.pad44 = 0;
}

Havok::StateInfoLayout* MakeState(const char* a_name, std::int32_t a_stateId, RE::hkbGenerator* a_gen,
    Havok::TransitionInfoArrayLayout* a_transitions) {
    auto* s = static_cast<Havok::StateInfoLayout*>(AllocZero(sizeof(RE::hkbStateMachine::StateInfo)));
    InitRefObj(s, RE::VTABLE_hkbStateMachine__StateInfo);
    {  // hkbBindable 부분만 (StateInfo 는 hkbNode 가 아니다)
        auto* b = reinterpret_cast<RE::hkbBindable*>(s);
        std::memset(&b->variableBindingSet, 0, sizeof(b->variableBindingSet));
        EmptyArray(b->cachedBindables);
        b->areBindablesCached = false;
    }
    EmptyArray(s->listeners);
    s->enterNotifyEvents = nullptr;
    s->exitNotifyEvents = nullptr;
    s->transitions = reinterpret_cast<RE::hkStateMachine_TransitionInfoArray_t*>(a_transitions);
    s->generator = a_gen;
    AddRef(a_gen);
    MakeString(s->name, a_name);
    s->stateId = a_stateId;
    s->probability = 1.0f;
    s->enable = true;
    return s;
}

RE::hkReferencedObject* MakeBlendEffect(
    const RE::hkReferencedObject* a_template, const char* a_name, float a_duration, int a_eventMode) {
    // 배치 (SSE 패키지 파일 실측 2026-09-05): hkbNode 0x48 | selfTransitionMode i8 @48 | eventMode i8 @49 | (8바이트 정렬 패딩) |
    //   duration f @50 | toGeneratorStartTimeFraction f @54 | flags u16 @58 | endMode i8 @5A | blendCurve i8 @5B |
    //   (런타임) fromGenerator @60 toGenerator @68 characterPose hkArray @70 timeRemaining @80 timeInTransition @84 applySelf @88 initPose @89 → 0x90
    //   ★이전 가정(duration @4C)은 틀렸었다 — MT DefaultBlendTransition(0.2s) 가 0.00 으로 읽혔다.
    constexpr std::size_t kSize = 0x90, kDuration = 0x50, kEventMode = 0x49, kSelfMode = 0x48, kRuntime = 0x60,
                          kPoseArray = 0x70;
    if (!a_template || !SameVTable(a_template, RE::VTABLE_hkbBlendingTransitionEffect)) return nullptr;
    const auto* src = reinterpret_cast<const std::uint8_t*>(a_template);
    const float tDur = *reinterpret_cast<const float*>(src + kDuration);
    const std::int8_t tSelf = static_cast<std::int8_t>(src[kSelfMode]),
                      tEvent = static_cast<std::int8_t>(src[kEventMode]);
    if (!(tDur >= 0.0f && tDur <= 5.0f) || tSelf < 0 || tSelf > 3 || tEvent < 0 || tEvent > 3) {
        logger::warn(
            "[Node] 블렌드 효과 배치 자가검증 실패 (template '{}' duration={} self={} event={}) — 기존 효과 재사용",
            NodeName(a_template), tDur, tSelf, tEvent);
        return nullptr;
    }
    auto* e = static_cast<std::uint8_t*>(AllocZero(kSize));
    std::memcpy(e, src, kSize);
    InitRefObj(e, RE::VTABLE_hkbBlendingTransitionEffect);
    {  // hkbBindable/hkbNode 런타임 필드 초기화 + 새 이름
        auto* b = reinterpret_cast<RE::hkbBindable*>(e);
        std::memset(&b->variableBindingSet, 0, sizeof(b->variableBindingSet));
        EmptyArray(b->cachedBindables);
        b->areBindablesCached = false;
        auto* n = reinterpret_cast<RE::hkbNode*>(e);
        n->userData = 0;
        MakeString(n->name, a_name);
    }
    *reinterpret_cast<float*>(e + kDuration) = std::max(0.0f, a_duration);
    if (a_eventMode >= 0 && a_eventMode <= 3) e[kEventMode] = static_cast<std::uint8_t>(a_eventMode);
    std::memset(e + kRuntime, 0, kSize - kRuntime);
    auto* arr = reinterpret_cast<RE::hkArray<RE::hkRefVariant>*>(e + kPoseArray);
    EmptyArray(*arr);
    logger::debug("[Node] 블렌드 효과 '{}' 생성 (template '{}' {:.2f}s → {:.2f}s, self {} eventMode {}→{})", a_name,
        NodeName(a_template), tDur, a_duration, tSelf, tEvent, a_eventMode >= 0 ? a_eventMode : tEvent);
    return reinterpret_cast<RE::hkReferencedObject*>(e);
}

Effects FindEffects(RE::hkbStateMachine* a_sm) {
    std::unordered_map<std::string, RE::hkReferencedObject*> byName;
    RE::hkReferencedObject* first = nullptr;
    auto scan = [&](RE::hkbStateMachine::TransitionInfoArray* a_arr) {
        if (!a_arr) return;
        auto* lay = reinterpret_cast<Havok::TransitionInfoArrayLayout*>(a_arr);
        for (int i = 0; i < lay->transitions._size; ++i) {
            auto* eff = lay->transitions._data[i].transition;
            if (!eff) continue;
            if (!first) first = eff;
            byName.emplace(NodeName(eff), eff);
        }
    };
    scan(a_sm->wildcardTransitions.get());
    for (int i = 0; i < a_sm->states._size; ++i) {
        auto* st = reinterpret_cast<Havok::StateInfoLayout*>(a_sm->states._data[i]);
        if (st) scan(reinterpret_cast<RE::hkbStateMachine::TransitionInfoArray*>(st->transitions));
    }
    auto pick = [&](std::initializer_list<const char*> a_prefs) -> std::pair<RE::hkReferencedObject*, std::string> {
        for (const char* p : a_prefs) {
            auto it = byName.find(p);
            if (it != byName.end()) return {it->second, p};
        }
        for (auto& [n, e] : byName)
            if (n.find("ToAnimationDriven") != std::string::npos) return {e, n};
        for (auto& [n, e] : byName)
            if (n.find("DefaultBlend") != std::string::npos) return {e, n};
        return {first, first ? NodeName(first) : ""};
    };
    Effects e;
    std::tie(e.enter, e.enterName) =
        pick({"DefaultBlendTransitionToAnimationDriven", "DefaultBlendTransition", "fastBlendTransition"});
    std::tie(e.exit, e.exitName) =
        pick({"DefaultBlendTransitionFromAnimationDriven", "DefaultBlendTransition", "fastBlendTransition"});
    return e;
}

}  // namespace AnimationInjector::NodeFactory
