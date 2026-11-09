#include "PCH.h"
#include "core/ExitBlendHooks.h"
#include "core/AirControl.h"
#include "core/GraphInjector.h"
#include "core/NodeIdRenumber.h"
#include "core/LoadingProgress.h"
#include "core/NodeFactory.h"
#include "core/AnimationLimit.h"
#include "core/AnimDataRegistry.h"
#include "core/FullBodyEntry.h"
#include "core/PairEntry.h"
#include "runtime/ActionRegistry.h"
#include "Settings.h"
#include "core/BoundedNodeSearch.h"
#include <atomic>
#include <bit>
#include <format>
#include <functional>
#include <map>
#include <tuple>
#include <mutex>
#include <shared_mutex>

namespace AnimationInjector::GraphInjector {

using namespace NodeFactory;
using Havok::TransitionInfo;
using Havok::StateInfoLayout;
using Havok::TransitionInfoArrayLayout;

namespace {
// 대상 루트 상태머신 (Behavior Atlas 실측값 — 자가검증에 쓴다). Both views' masters pass it: the first-person
// root has 20 states, the same start state and wildcard[0]. Ordinary actions join its FullBodyPlayerIdles machine in
// both views (0.12.232); the weapon graphs (1HM_Behavior, MT_RootBehavior, MagicBehavior) that first person used
// to get are no longer targets, so an action plays in first person whatever the weapon state.
struct Target {
    const char* smName;
    std::int32_t startStateId;
    std::int32_t minStates;
    const char* startStateName;
    const char* firstWildcardEvent;
};
constexpr Target kTargets[] = {
    // The third-person master contains the native paired states. PairB does
    // not add placeholder nodes to the unrelated first-person graph.
    {"RootBehaviorGraph", 14, 20, "Default_State", "HorseEnterInstant"},
};
thread_local bool g_currentFirstPerson = false;  // SymbolHook 의 a_path 에 "_1stPerson" 이 있으면 (삽입 노드 표시용)

// ── One injection pass (0.12.208) ──────────────────────────────────────────────────────────────
// TryInjectGraph adds every declared action to one graph. Each action used to search the native
// templates again (named states, the full-body entry and exit, transition effects, the used state
// ids) in arrays that grew with every added action, and the full-body route copied the whole
// parent exit list for each action. With thousands of actions a pass became quadratic in time and
// memory. A pass now looks each template up once; injected nodes never carry the native template
// names, so the first lookup finds what every later one would. The full-body exits are staged and
// written once when the pass ends; each action still stages all of its exits or none.
struct Pass {
    std::map<std::pair<const void*, std::string>, StateInfoLayout*> named;
    std::unordered_map<const void*, Effects> effects;
    struct Ids {
        std::unordered_set<std::int32_t> used;  // filled by PassIds; TryInjectGraph keeps its own root set
        std::int32_t next = 0;                  // TakeStateIds cursor; 0 = not started
        bool filled = false;
    };
    std::unordered_map<const void*, Ids> ids;
    // Blends by (native template, duration bits, event mode, exit): one immutable effect each (SharedEffect).
    std::map<std::tuple<const void*, std::uint32_t, int, bool>, RE::hkReferencedObject*> sharedEffects;
    // Runs once the staged exits and pair containers have joined the graph (the node id budget counts then).
    std::function<void()> afterCommit;
    struct FullBody {
        bool resolved = false;
        const char* failure = nullptr;
        RE::hkbBehaviorGraph* graph = nullptr;
        StateInfoLayout* parent = nullptr;
        RE::hkbStateMachine* machine = nullptr;
        StateInfoLayout* nativeIdle = nullptr;
        TransitionInfo entry{}, exit{};
        std::int32_t idleStop = -1;         // IdleStop's event id: an exit staged with it becomes the template
        std::vector<TransitionInfo> exits;  // staged parent exits, referenced (AddRef) when staged
        bool failureReported = false;       // the failure is logged once per graph
        float nativeBlend = 0;              // the native entry blend's duration
    } fullBody;
    // The shared pair containers (core/PairEntry.h), built at the first paired action of a character
    // graph. They join the root when the pass ends, each only when it holds a pair.
    struct Pairs {
        bool resolved = false;
        const char* failure = nullptr;  // containers unavailable: every pair keeps its own root states
        RE::hkbBehaviorGraph* graph = nullptr;
        RE::hkbStateMachine* root = nullptr;
        TransitionInfoArrayLayout* wild = nullptr;
        std::int32_t reserved = -1;  // the first pair's nested id, reserved with the containers
        struct Role {
            std::int32_t id = -1;                  // the container's root state id
            StateInfoLayout* container = nullptr;  // root state around the nested machine
            RE::hkbStateMachine* machine = nullptr;
            TransitionInfoArrayLayout* innerWild = nullptr;
            TransitionInfoArrayLayout* exits = nullptr;        // the container's exits
            TransitionInfo entry{};                            // the shared root entry
            TransitionInfo exit{};                             // the native role exit (PairEnd / pairedStop)
            std::array<RE::hkReferencedObject*, 3> effects{};  // by PairExitKind
            float nativeExit = 0;                              // the native role exit's duration
            std::unordered_map<std::int32_t, int> exitKinds;   // staged exit event -> PairExitKind
            std::int32_t first = -1;                           // the first nested state (start state)
            int states = 0;
        };
        std::array<Role, 2> roles;  // lead (index 0, clips[0]), support (index 1)
    } pairs;
};
thread_local Pass* g_pass = nullptr;
// A blend effect: every enter, exit, recovery, cancel and link blend in this graph with the same native template,
// duration and event mode shares one effect. Nothing changes an effect after it is made, and native graphs share
// theirs the same way. Each effect is a node and takes one of the graph's 16-bit node ids: one per link grew with the
// square of the linked actions (11-07 Trung53: 98 actions linked to one another made 6072 effects and pushed a
// 59520-node graph past 65535, the ids wrapped and a global transition of the cart driver read a null state
// machine), and one enter, exit and recovery effect per action made three of an action's four nodes (11-08).
// Exit effects are kept apart from the others (ExitBlendHooks knows them by name and hands movement back during them:
// RegisterSharedExit); an exit effect used as an enter would hand movement back at an action's start. A creature pair
// keeps its own exit effect: ExitBlendHooks finds that pair by the effect's name.
RE::hkReferencedObject* SharedEffect(
    const RE::hkReferencedObject* a_template, float a_seconds, int a_eventMode, bool a_exit = false) {
    const float seconds = std::max(0.0f, a_seconds);
    const auto make = [&] {
        return MakeBlendEffect(a_template,
            std::format("{}_{:.2f}s{}", a_exit ? "AI_ExitBlend" : "AI_Blend", seconds, a_eventMode > 0 ? "_e" : "")
                .c_str(),
            seconds, a_eventMode);
    };
    if (!a_template) return nullptr;
    if (!g_pass) return make();
    const auto key =
        std::tuple{static_cast<const void*>(a_template), std::bit_cast<std::uint32_t>(seconds), a_eventMode, a_exit};
    if (const auto it = g_pass->sharedEffects.find(key); it != g_pass->sharedEffects.end()) return it->second;
    auto* effect = make();
    if (effect) g_pass->sharedEffects.emplace(key, effect);
    return effect;
}
RE::hkReferencedObject* SharedLinkEffect(const RE::hkReferencedObject* a_template, float a_seconds, int a_eventMode) {
    return SharedEffect(a_template, a_seconds, a_eventMode);
}
// The container exits by the blend variable their effect reads (core/PairEntry.h).
enum PairExitKind : int { kPairExit, kPairLoss, kPairRecovery };
void CommitPairContainers(Pass::Pairs& pairs);
// The last state of the machine with this name, as the former per-action scans returned it.
StateInfoLayout* NamedState(RE::hkbStateMachine* sm, const char* name) {
    auto find = [&] {
        StateInfoLayout* found = nullptr;
        for (int k = 0; k < sm->states._size; ++k) {
            auto* state = reinterpret_cast<StateInfoLayout*>(sm->states._data[k]);
            if (state && state->name.c_str() && std::strcmp(state->name.c_str(), name) == 0) found = state;
        }
        return found;
    };
    if (!g_pass) return find();
    const auto key = std::make_pair(static_cast<const void*>(sm), std::string(name));
    if (const auto it = g_pass->named.find(key); it != g_pass->named.end()) return it->second;
    return g_pass->named.emplace(key, find()).first->second;
}
const Effects& PassEffects(RE::hkbStateMachine* sm) {
    static thread_local Effects fallback;
    if (!g_pass) return fallback = FindEffects(sm);
    if (const auto it = g_pass->effects.find(sm); it != g_pass->effects.end()) return it->second;
    return g_pass->effects.emplace(sm, FindEffects(sm)).first->second;
}
// The machine's used state ids, read from its states once per pass and kept current by the callers.
std::unordered_set<std::int32_t>& PassIds(RE::hkbStateMachine* sm) {
    static thread_local Pass::Ids fallback;
    auto& entry = g_pass ? g_pass->ids[sm] : (fallback = {});
    if (!entry.filled) {
        for (int k = 0; k < sm->states._size; ++k)
            if (auto* state = reinterpret_cast<StateInfoLayout*>(sm->states._data[k]))
                entry.used.insert(state->stateId);
        entry.filled = true;
    }
    return entry.used;
}
// A block of n unused state ids in the machine, handed out densely from iStateIdBase (ids used to
// be spaced 100 per action index, which ran out after about 220 actions). -1 when the block would
// pass 32767: larger ids were never exercised by the engine.
std::int32_t TakeStateIds(RE::hkbStateMachine* sm, const std::unordered_set<std::int32_t>& used, int n) {
    if (n <= 0) return -1;
    static thread_local std::int32_t fallbackNext = 0;
    std::int32_t* next = &fallbackNext;
    if (g_pass) {
        auto& entry = g_pass->ids[sm];
        if (entry.next == 0) entry.next = FrameworkSettings::Get().stateIdBase;
        next = &entry.next;
    } else
        fallbackNext = FrameworkSettings::Get().stateIdBase;
    std::int32_t base = std::max(FrameworkSettings::Get().stateIdBase, *next);
    for (;;) {
        if (base > 32767 - (n - 1)) return -1;
        std::int32_t clash = -1;
        for (int i = 0; i < n; ++i)
            if (used.contains(base + i)) clash = base + i;
        if (clash < 0) break;
        base = clash + 1;
    }
    *next = base + n;
    return base;
}
// The parent's exit list becomes its exits followed by the staged ones in staging order: what the
// former per-action copies ended with. The staged transitions were referenced when staged.
void CommitFullBodyExits(Pass::FullBody& body) {
    if (body.exits.empty() || !body.parent) return;
    auto* current = reinterpret_cast<TransitionInfoArrayLayout*>(body.parent->transitions);
    auto* exits = MakeTransitionArray(0);
    if (current)
        for (int i = 0; i < current->transitions._size; ++i) {
            auto t = current->transitions._data[i];
            AddRef(t.transition);
            AddRef(t.condition);
            exits->transitions.push_back(t);
        }
    for (const auto& t : body.exits) exits->transitions.push_back(t);
    body.exits.clear();
    body.parent->transitions = reinterpret_cast<RE::hkStateMachine_TransitionInfoArray_t*>(exits);
}
// Makes a Pass current for one TryInjectGraph call; the staged full-body exits are written when it ends.
struct PassScope {
    Pass pass;
    Pass* previous = g_pass;
    PassScope() { g_pass = &pass; }
    ~PassScope() {
        CommitFullBodyExits(pass.fullBody);
        CommitPairContainers(pass.pairs);
        if (pass.afterCommit) pass.afterCommit();
        g_pass = previous;
    }
    PassScope(const PassScope&) = delete;
    PassScope& operator=(const PassScope&) = delete;
};

// Graph loading runs on loader threads (the injection profile logs their ids) while main and
// animation threads query these tables. Writers hold the exclusive lock only for the append;
// no engine call is made while either lock is held.
std::shared_mutex g_publishLock;  // g_results, g_syncNodes, nativeTransitionEvents
std::vector<Result> g_results;
std::unordered_map<int, std::vector<std::size_t>> g_resultsByAction;  // action -> g_results indexes
std::vector<SyncNode> g_syncNodes;
std::vector<std::string> nativeTransitionEvents;
void Publish(Result result) {
    std::unique_lock lock(g_publishLock);
    g_resultsByAction[result.actionIdx].push_back(g_results.size());
    g_results.push_back(std::move(result));
}
// Results of one action, oldest first. Caller holds g_publishLock.
template<class F>
bool AnyResultOf(int action, F&& f) {
    const auto it = g_resultsByAction.find(action);
    if (it == g_resultsByAction.end()) return false;
    for (const auto index : it->second)
        if (f(g_results[index])) return true;
    return false;
}
void PublishSync(SyncNode node) {
    std::unique_lock lock(g_publishLock);
    g_syncNodes.push_back(std::move(node));
}
// Graphs already handled. Claim() is the atomic check-and-insert between loader threads.
// A graph is known by its address with its data and root generator: a project loaded again can get a new graph at
// a freed graph's address, and that one has not been injected (2026-10-28 review R-22; EventMaps re-checks the
// same way).
struct Handled {
    const void* data = nullptr;
    const void* root = nullptr;
};
std::mutex g_doneLock;
std::unordered_map<const void*, Handled> g_done;
Handled HandledOf(RE::hkbBehaviorGraph* graph) { return {graph->data.get(), graph->rootGenerator.get()}; }
bool Claimed(RE::hkbBehaviorGraph* graph) {
    const auto now = HandledOf(graph);
    std::lock_guard lock(g_doneLock);
    const auto it = g_done.find(graph);
    if (it == g_done.end()) return false;
    if (it->second.data == now.data && it->second.root == now.root) return true;
    logger::info("[Inject] graph at {:X} was replaced (new data or root); injecting again",
        reinterpret_cast<std::uintptr_t>(graph));
    g_done.erase(it);
    return false;
}
bool Claim(RE::hkbBehaviorGraph* graph) {
    const auto now = HandledOf(graph);
    std::lock_guard lock(g_doneLock);
    return g_done.try_emplace(graph, now).second;
}

int RegisterAnimationFiles(RE::hkbCharacterStringData* strings, const char* stage) {
    if (!strings) return 0;
    auto& names = strings->animationNames;
    const int before = names._size;
    int added = 0;
    NameLookupScope nameLookup(names);
    const auto rig = PairedRig::CharacterName(strings->name.c_str() ? strings->name.c_str() : "");
    // Register the complete declaration list before any graph can append a
    // subset in traversal order. Shared Male/Female templates must not get
    // different indices just because their registration paths differ.
    // Each view's project takes the files of the actions that play in it (runtime/ViewSpec.h); first
    // person never gets a paired state.
    const auto& files = PairedRig::Equal(strings->name.c_str() ? strings->name.c_str() : "", "FirstPerson")
                            ? Actions::FirstPersonClipFiles()
                            : Actions::AllClipFiles(rig);
    bool full = false;
    for (const auto& file : files) {
        if (FindName(names, file.c_str()) >= 0) continue;
        if (AnimationTableFull(names, strings->name.c_str())) {
            full = true;
            break;
        }
        LoadingProgress::Registration loading;
        if (!AppendName(names, file.c_str())) {
            loading.Fail();
            logger::error("[Inject] {} animation registration failed project='{}' file='{}'", stage,
                strings->name.c_str() ? strings->name.c_str() : "?", file);
            break;
        }
        ++added;
    }
    const char* project = strings->name.c_str();
    // The loading panel's animation row: the character projects (both views), whose tables the actions fill.
    if (!files.empty() &&
        (rig == PairedRig::Kind::kCharacter || PairedRig::Equal(project ? project : "", "FirstPerson")))
        LoadingProgress::ReportAnimations(static_cast<std::size_t>(std::max(0, names._size)),
            static_cast<std::size_t>(AnimationLimit::Get().bindingCap), full);
    if (added && project && rig != PairedRig::Kind::kUnsupported)
        logger::debug("[Inject] {} animation registration project='{}' names {} -> {} added={} order=declaration",
            stage, project, before, names._size, added);
    return added;
}

bool Contains(const char* a_hay, const char* a_needle) {
    if (!a_hay || !a_needle) return false;
    const std::size_t n = std::strlen(a_needle);
    for (const char* c = a_hay; *c; ++c)
        if (_strnicmp(c, a_needle, n) == 0) return true;
    return false;
}

// Resolve unsupported wrapper fields through Havok's own reflection metadata.
// No guessed structure offsets or engine function signatures are used here.
template<class T>
T* ReflectedPointer(RE::hkbNode* node, const char* name) {
    if (!node) return nullptr;
    const auto* concrete = node->GetClassType();
    if (!concrete || concrete->objectSize < sizeof(RE::hkbNode) || concrete->objectSize > 4096) return nullptr;
    for (const auto* cls = concrete; cls; cls = cls->parent) {
        if (cls->numDeclaredMembers < 0 || cls->numDeclaredMembers > 256 ||
            (cls->numDeclaredMembers && !cls->declaredMembers))
            return nullptr;
        for (int i = 0; i < cls->numDeclaredMembers; ++i) {
            const auto& member = cls->declaredMembers[i];
            if (member.m_name && std::strcmp(member.m_name, name) == 0 &&
                member.m_type == RE::hkClassMember::TYPE_POINTER &&
                member.m_offset + sizeof(T) <= static_cast<std::size_t>(concrete->objectSize))
                return reinterpret_cast<T*>(reinterpret_cast<std::byte*>(node) + member.m_offset);
        }
    }
    return nullptr;
}

RE::hkbNode* CopyPairedWrapper(RE::hkbNode* source, const char* name) {
    if (!SameVTable(source, RE::VTABLE_hkbModifierGenerator)) return nullptr;
    const auto* cls = source ? source->GetClassType() : nullptr;
    if (!cls || cls->objectSize < sizeof(RE::hkbNode) || cls->objectSize > 4096) {
        logger::warn("[PairB] wrapper copy rejected: node='{}' class='{}' size={}", NodeName(source),
            cls && cls->name ? cls->name : "(none)", cls ? cls->objectSize : 0);
        return nullptr;
    }
    auto* node = static_cast<RE::hkbNode*>(AllocZero(cls->objectSize));
    if (!node) {
        logger::error("[PairB] wrapper allocation failed: bytes={}", cls->objectSize);
        return nullptr;
    }
    std::memcpy(node, source, cls->objectSize);
    node->memSizeAndFlags = 0xFFFF;
    node->referenceCount = 0x4000;
    EmptyArray(node->cachedBindables);
    node->areBindablesCached = false;
    if (node->variableBindingSet) AddRef(node->variableBindingSet.get());
    MakeString(node->name, name);
    // Serialized node identities are assigned when the modified graph is linked.
    node->id = 0xFFFF;
    node->cloneState = RE::hkbNode::CloneState::kDefault;
    return node;
}

void LogReflectedFields(RE::hkbNode* node) {
    if (!FrameworkSettings::Get().debug) return;
    const auto* concrete = node ? node->GetClassType() : nullptr;
    logger::debug("[PairB] reflection node='{}' class='{}' size={}", NodeName(node),
        concrete && concrete->name ? concrete->name : "(none)", concrete ? concrete->objectSize : 0);
    int depth = 0;
    for (const auto* cls = concrete; cls && depth < 16; cls = cls->parent, ++depth) {
        if (cls->numDeclaredMembers < 0 || cls->numDeclaredMembers > 256 ||
            (cls->numDeclaredMembers && !cls->declaredMembers))
            return;
        for (int i = 0; i < cls->numDeclaredMembers; ++i) {
            const auto& member = cls->declaredMembers[i];
            logger::debug("[PairB] reflection member='{}' type={} subtype={} offset={} arraySize={}",
                member.m_name ? member.m_name : "(none)", static_cast<int>(member.m_type.get()),
                static_cast<int>(member.m_subtype.get()), member.m_offset, member.m_cArraySize);
        }
    }
}

const char* PreparePairContainers(RE::hkbBehaviorGraph* graph, RE::hkbStateMachine* root,
    TransitionInfoArrayLayout* wild, std::unordered_set<std::int32_t>& ids, int startStateId,
    const std::array<StateInfoLayout*, 2>& source, const std::array<TransitionInfo, 2>& entry, Pass::Pairs& pairs);
void StagePairExit(RE::hkbBehaviorGraph* graph, Pass::Pairs::Role& role, TransitionInfo transition, int kind);

void InjectPairedAction(int actionIndex, RE::hkbBehaviorGraph* graph, RE::hkbStateMachine* sm,
    TransitionInfoArrayLayout* wild, std::unordered_set<std::int32_t>& ids, RE::hkbCharacter* character,
    int startStateId) {
    const auto& spec = Actions::Spec(actionIndex);
    auto skip = [&](const char* reason) {
        logger::warn("[PairB] '{}' injection skipped: {} (firstPerson={} clips={})", spec.id, reason,
            g_currentFirstPerson, spec.clips.size());
    };
    if (g_currentFirstPerson) return skip("first-person graph");
    LoadingProgress::Registration loading(LoadingProgress::Outcome::kFailed);
    // Pairs live in the containers of an injection pass; once the pass found none (logged once), the
    // graph gets no pair.
    if (!g_pass || (g_pass->pairs.resolved && g_pass->pairs.failure)) return;
    if (spec.clips.size() != 2 || !wild) return skip("two roles and wildcard transitions required");
    logger::debug("[PairB] '{}' validating native paired templates", spec.id);
    std::array<StateInfoLayout*, 2> source{};
    std::array<RE::BSSynchronizedClipGenerator*, 2> sourceSync{};
    std::array<RE::hkbNode*, 2> sourceModifier{};
    std::array<TransitionInfo, 2> sourceEntry{};
    const std::array<const char*, 2> names{"VampireFeedStanding_Front_Victim", "VampireFeedStanding_Front_Player"};
    for (int i = 0; i < 2; ++i) {
        source[i] = NamedState(sm, names[i]);
        auto* wrapper = source[i] ? source[i]->generator : nullptr;
        if (!wrapper || !SameVTable(wrapper, RE::VTABLE_hkbModifierGenerator)) {
            logger::warn("[PairB] reference wrapper missing: {}", names[i]);
            return;
        }
        auto** gen = ReflectedPointer<RE::hkbGenerator*>(wrapper, "generator");
        auto** modifier = ReflectedPointer<RE::hkbNode*>(wrapper, "modifier");
        if (!gen || !*gen || !SameVTable(*gen, RE::VTABLE_BSSynchronizedClipGenerator) || !modifier || !*modifier ||
            !SameVTable(*modifier, RE::VTABLE_BSIsActiveModifier)) {
            logger::warn(
                "[PairB] reference role={} generatorField={} generator='{}' syncType={} modifierField={} modifier='{}' activeType={}",
                i, gen != nullptr, gen ? NodeName(*gen) : "(none)",
                gen && SameVTable(*gen, RE::VTABLE_BSSynchronizedClipGenerator), modifier != nullptr,
                modifier ? NodeName(*modifier) : "(none)",
                modifier && SameVTable(*modifier, RE::VTABLE_BSIsActiveModifier));
            LogReflectedFields(wrapper);
            return skip("reference generator/modifier fields or types mismatch");
        }
        sourceSync[i] = static_cast<RE::BSSynchronizedClipGenerator*>(*gen);
        sourceModifier[i] = *modifier;
        const char* prefix = sourceSync[i]->syncAnimPrefix.c_str();
        std::uintptr_t prefixBits = 0;
        static_assert(sizeof(prefixBits) == sizeof(sourceSync[i]->syncAnimPrefix));
        std::memcpy(&prefixBits, &sourceSync[i]->syncAnimPrefix, sizeof(prefixBits));
        logger::debug("[PairB] reference prefix role={} null={} managed={}", i, prefixBits == 0,
            (prefixBits & RE::hkStringPtr::kManaged) != 0);
        if (prefixBits & RE::hkStringPtr::kManaged)
            return skip("native reference prefix is managed; raw prefix access would be unsafe");
        logger::debug("[PairB] reference role={} state='{}' sync='{}' prefix='{}' lead={} clip={} id={} clone={}", i,
            names[i], NodeName(sourceSync[i]), prefix ? prefix : "", sourceSync[i]->leadCharacter,
            sourceSync[i]->clipGenerator != nullptr, sourceSync[i]->id,
            static_cast<int>(sourceSync[i]->cloneState.get()));
        if (!sourceSync[i]->clipGenerator || sourceSync[i]->leadCharacter != (i == 0))
            return skip("reference clip or role mismatch");
        if (std::strcmp(prefix ? prefix : "", i == 0 ? "2_" : "") != 0) return skip("reference prefix mismatch");
        bool found = false;
        for (int k = 0; k < wild->transitions._size; ++k) {
            const auto& entry = wild->transitions._data[k];
            if (entry.toStateId == source[i]->stateId &&
                std::strcmp(EventNameOf(graph, entry.eventId), sourceSync[i]->name.c_str()) == 0) {
                sourceEntry[i] = entry;
                found = true;
                break;
            }
        }
        if (!found || !source[i]->transitions) {
            logger::warn("[PairB] reference transitions missing");
            return;
        }
    }
    // The native front-feed wrappers already share an activity modifier with
    // other paired states. Retain those same-graph references and let native
    // graph cloning manage the modifier; it does not need a private copy.
    logger::debug("[PairB] native activity modifiers sharedAcrossRoles={}", sourceModifier[0] == sourceModifier[1]);
    std::array<RE::hkbNode*, 2> newWrappers{};
    for (int i = 0; i < 2; ++i) {
        const auto* cls = sourceModifier[i]->GetClassType();
        logger::debug("[PairB] role={} retain native modifier='{}' class='{}' reflectedSize={} bindingSet={}", i,
            NodeName(sourceModifier[i]), cls && cls->name ? cls->name : "(none)", cls ? cls->objectSize : 0,
            sourceModifier[i]->variableBindingSet.get() != nullptr);
        auto* wrapper = CopyPairedWrapper(source[i]->generator, (spec.clips[i].enterEvent + "_Wrapper").c_str());
        if (!wrapper) return skip("reference wrapper copy failed");
        auto** child = ReflectedPointer<RE::hkbGenerator*>(wrapper, "generator");
        auto** modifier = ReflectedPointer<RE::hkbNode*>(wrapper, "modifier");
        if (!child || !modifier || *child != sourceSync[i] || *modifier != sourceModifier[i]) {
            LogReflectedFields(wrapper);
            return skip("copied wrapper links mismatch");
        }
        newWrappers[i] = wrapper;
    }
    auto* setup = character ? character->setup.get() : nullptr;
    auto* data = setup ? setup->data.get() : nullptr;
    auto* strings = data ? data->stringData.get() : nullptr;
    if (!strings) return skip("character string data missing");
    // The pair containers (core/PairEntry.h), prepared at the first pair of the pass from the native
    // role states validated above.
    auto& pairs = g_pass->pairs;
    if (!pairs.resolved) {
        pairs.resolved = true;
        pairs.failure = PreparePairContainers(graph, sm, wild, ids, startStateId, source, sourceEntry, pairs);
        if (pairs.failure) {
            logger::error(
                "[PairEntry] pair containers unavailable ({}); paired actions are not injected into this graph",
                pairs.failure);
            return;
        }
    }
    // One nested id, the same in both containers, taken from the root's ids so that it is never a key of
    // the state map the containers point at.
    const int base = pairs.reserved >= 0 ? std::exchange(pairs.reserved, -1) : TakeStateIds(sm, ids, 1);
    if (base < 0) return skip("state IDs exhausted");
    ids.insert(base);
    Result result;
    result.actionIdx = actionIndex;
    result.graph = "RootBehaviorGraph";
    result.pairedRig = PairedRig::Kind::kCharacter;
    result.enterIds.resize(2, -1);
    std::array<StateInfoLayout*, 2> newStates{};
    std::array<RE::BSSynchronizedClipGenerator*, 2> newSync{};
    // The container exits this pair needs and the blends its entry sets, by role.
    struct StagedExit {
        TransitionInfo transition;
        int kind;
    };
    std::array<std::vector<StagedExit>, 2> staged;
    std::array<PairEntry::Blends, 2> blends{};
    for (int i = 0; i < 2; ++i) {
        if (!PairedRig::SupportsRole(spec.rig, spec.paired.targetRig, PairedRig::Kind::kCharacter, i)) continue;
        const auto& variant = spec.clips[i];
        const auto& role = pairs.roles[i];
        const int enter = InjectEvent(graph, variant.enterEvent.c_str());
        const int end = InjectEvent(graph, i == 0 ? "PairEnd" : "pairedStop");
        const int prefixedEnd = InjectEvent(graph, i == 0 ? "2_PairEnd" : "NPCpairedStop");
        const int customEnd = InjectEvent(graph, spec.events.exit.c_str());
        if (enter < 0 || end < 0 || prefixedEnd < 0 || customEnd < 0) return skip("event registration failed");
        const int binding = EnsureAnimationName(character, variant.file.c_str());
        if (binding < 0 || binding > AnimationLimit::MaxBindingIndex())
            return skip("animation index unavailable or out of range");
        // Preserve annotation conversion and use clip-relative completion,
        // rather than copying the original 3.467-second clip's trigger times.
        std::vector<TriggerDef> pairTriggers;
        const float exitAt = spec.exitTriggerBeforeEnd;
        if (exitAt >= 0) pairTriggers.push_back({exitAt, customEnd, true});
        auto* clip = MakeClip(
            variant.clipName.c_str(), variant.file.c_str(), binding, MakeTriggerArray(pairTriggers), nullptr, true);
        auto* ref = sourceSync[i];
        auto* sync = static_cast<RE::BSSynchronizedClipGenerator*>(MakeSyncClip(variant.enterEvent.c_str(), clip,
            i == 0 ? "2_" : "", i == 0, ref->getToMarkTime, ref->markErrorThreshold, ref->reorientSupportChar,
            ref->applyMotionFromRoot, nullptr, &ref->syncAnimPrefix));
        auto* modifier = sourceModifier[i];
        auto* wrapper = newWrappers[i];
        auto** child = ReflectedPointer<RE::hkbGenerator*>(wrapper, "generator");
        auto** mod = ReflectedPointer<RE::hkbNode*>(wrapper, "modifier");
        if (!child || !mod) return skip("copied wrapper fields unavailable");
        *child = sync;
        AddRef(sync);
        *mod = modifier;
        AddRef(modifier);
        // The native role state's exits back to the root's start state become container exits.
        auto* sourceTransitions = reinterpret_cast<TransitionInfoArrayLayout*>(source[i]->transitions);
        bool haveExit = false;
        for (int k = 0; k < sourceTransitions->transitions._size; ++k) {
            const auto& transition = sourceTransitions->transitions._data[k];
            if (transition.toStateId != startStateId) continue;
            haveExit |= transition.eventId == end;
            staged[i].push_back({transition, kPairExit});
        }
        if (!haveExit) return skip("standard paired exit missing");
        // The container's exits blend for the seconds this pair's entry sets on the actor.
        const float recovery = spec.recovery.blend >= 0 ? spec.recovery.blend
                               : spec.exitBlend >= 0    ? spec.exitBlend
                                                        : 0.2f;
        blends[i] = {spec.exitBlend >= 0 ? spec.exitBlend : role.nativeExit,
            spec.paired.lossBlend > 0.0f ? spec.paired.lossBlend : role.nativeExit,
            spec.recovery.enabled ? recovery : role.nativeExit};
        if (spec.paired.lossBlend > 0.0f) {
            const int lossExit = InjectEvent(graph, spec.paired.lossExitEvent.c_str());
            if (lossExit < 0) return skip("partner-loss exit event unavailable");
            auto t = role.exit;
            t.eventId = lossExit;
            staged[i].push_back({t, kPairLoss});
            logger::debug("[PairLossBlend] '{}' role={} container={} exit={} blend={:.2f}", spec.id, i, role.id,
                lossExit, spec.paired.lossBlend);
        }
        if (spec.recovery.enabled) {
            const int recover = InjectEvent(graph, spec.recovery.recoverEvent.c_str());
            if (recover < 0) return skip("paired recovery event unavailable");
            auto t = role.exit;
            t.eventId = recover;
            staged[i].push_back({t, kPairRecovery});
        }
        auto t = role.exit;
        t.eventId = customEnd;
        staged[i].push_back({t, kPairExit});
        // The role's notify events belong to the container state (core/PairEntry.h).
        newStates[i] = MakeState(
            variant.enterEvent.c_str(), base, reinterpret_cast<RE::hkbGenerator*>(wrapper), MakeTransitionArray(0));
        newSync[i] = sync;
        result.enterIds[i] = enter;
        // A pair's root state is its role's container.
        result.stateIds.push_back(role.id);
        result.exitId = customEnd;
    }
    // Commit both roles together. Native graph loading creates their bindings.
    for (int i = 0; i < 2; ++i) {
        if (!newStates[i]) continue;
        // Inside the role's container: the shared root entry reaches the state through iAI_PairState, the
        // inner wildcard while the container is still active (re-entry during an exit blend).
        auto& role = pairs.roles[i];
        auto entry = sourceEntry[i];
        entry.eventId = result.enterIds[i];
        entry.toStateId = base;
        AddRef(entry.transition);
        AddRef(entry.condition);
        role.machine->states.push_back(reinterpret_cast<RE::hkbStateMachine::StateInfo*>(newStates[i]));
        role.innerWild->transitions.push_back(entry);
        if (role.first < 0) role.first = base;
        ++role.states;
        for (const auto& exit : staged[i]) StagePairExit(graph, role, exit.transition, exit.kind);
        PairEntry::Register(spec.clips[i].enterEvent.c_str(),
            i == 0 ? PairEntry::Role::kLead : PairEntry::Role::kSupport, base, blends[i]);
        PublishSync({newSync[i], newSync[i]->clipGenerator, spec.clips[i].file, actionIndex, i, false});
        logger::debug(
            "[PairB] injected role={} container={} state={} event='{}' clip='{}' id={} clone={} referenceId={} "
            "referenceClone={}",
            i, role.id, base, spec.clips[i].enterEvent, spec.clips[i].file, newSync[i]->id,
            static_cast<int>(newSync[i]->cloneState.get()), sourceSync[i]->id,
            static_cast<int>(sourceSync[i]->cloneState.get()));
    }
    // The shared exit effect finds the action through the pair session (ExitBlendHooks).
    if (spec.exitBlend >= 0 || spec.paired.lossBlend > 0.0f) ExitBlendHooks::RegisterPairDestination(actionIndex);
    sm->areBindablesCached = false;
    graph->areBindablesCached = false;
    if (graph->rootGenerator) graph->rootGenerator->areBindablesCached = false;
    result.injected = true;
    Publish(std::move(result));
    loading.Ready();
}

struct HkChild {
    RE::hkbNode* node;
    void* info;
};
struct RawArr {
    void* data;
    std::int32_t size;
    std::int32_t capFlags;
};
struct ReadNodeName {
    const char* operator()(RE::hkbNode* node) const { return NodeName(node); }
};
// The node's children as the engine lists them (hkbNode virtuals 8 = their number, 9 = the list, no flags):
// generators, modifiers and transition effects. A node listing more than a_cap gives none.
int ListChildren(RE::hkbNode* node, std::vector<HkChild>& buffer, int a_cap) {
    using GetMaxFn = int (*)(RE::hkbNode*, int);
    using GetChildrenFn = void (*)(RE::hkbNode*, int, RawArr*);
    auto** vt = *reinterpret_cast<void***>(node);
    const int max = reinterpret_cast<GetMaxFn>(vt[8])(node, 0);
    if (max <= 0 || max > a_cap) return 0;
    buffer.assign(static_cast<std::size_t>(max), HkChild{});
    RawArr arr{buffer.data(), 0, static_cast<std::int32_t>(static_cast<std::uint32_t>(max) | kDontDealloc)};
    struct ChildrenOut {
        RawArr* arr;
        std::int32_t n;
        std::int32_t pad;
    } out{&arr, 0, 0};
    reinterpret_cast<GetChildrenFn>(vt[9])(node, 0, reinterpret_cast<RawArr*>(&out));
    return std::clamp(arr.size, 0, max);
}
struct ReadNodeChildren {
    int operator()(RE::hkbNode* node, std::vector<HkChild>& buffer) const { return ListChildren(node, buffer, 4096); }
};

// ── Node id budget ───────────────────────────────────────────────────────────
// The engine numbers a behavior graph's nodes with 16-bit ids when it links the graph (hkbNode::id, 0xFFFF = none;
// hkbBehaviorGraph::numStaticNodes and nextUniqueID are 16-bit too). Past 65535 nodes the ids wrap: two state
// machines share an id, the graph's id-to-machine table loses one, and a global transition through it reads null
// (11-07 Trung53, SE 1.5.97: a 59520-node 0_master + 6170 AI nodes = 65690, the engine's nextUniqueID read 154 and
// IdleCartDriverSway crashed in game+0xA1B2AB). Both numbers were measured: the native count below is what this walk
// returns, and the engine's count was exactly it plus the nodes NodeFactory made. Runtime clones take further ids
// after the static ones (nextUniqueID ran 3 past numStaticNodes in a test game), so some are kept free.
constexpr std::size_t kNodeIdLimit = 0xFFFF;  // ids 0..0xFFFE
constexpr std::size_t kNodeIdsKeptFree = 512;
std::size_t CountNodes(RE::hkbNode* root) {
    std::unordered_set<RE::hkbNode*> seen;
    std::vector<RE::hkbNode*> stack{root};
    std::vector<HkChild> buffer;
    while (!stack.empty()) {
        auto* node = stack.back();
        stack.pop_back();
        if (!node || !seen.insert(node).second) continue;
        const int count = ListChildren(node, buffer, 1 << 20);
        for (int i = count - 1; i >= 0; --i) stack.push_back(buffer[i].node);
    }
    return seen.size();
}
// The most nodes one action can make in a character graph: per clip its clip, a sync clip, a recovery effect and
// cancel-window effects; the action's enter, exit and air-route effects; one shared effect per incoming link at most.
std::size_t NodeReserve(int a_action) {
    const auto& spec = Actions::Spec(a_action);
    const std::size_t clips = spec.clips.size();
    return clips * (4 + 2 * spec.cancels.size()) + Actions::IncomingLinks(a_action).size() + 16;
}
using NativeNodeSearch = BoundedNodeSearch<RE::hkbNode, HkChild, ReadNodeName, ReadNodeChildren>;
RE::hkbNode* FindNodeByName(RE::hkbNode*, const char*, int, std::size_t&);

int EnsurePairFlag(RE::hkbBehaviorGraph* graph, const char* name) {
    auto* data = graph ? graph->data.get() : nullptr;
    if (!data || !data->stringData || !data->variableInitialValues) return -1;
    auto& names = data->stringData->variableNames;
    if (names._size != data->variableInfos._size) return -1;
    const int existing = FindName(names, name);
    if (existing >= 0)
        return data->variableInfos._data[existing].m_type == RE::hkbVariableInfo::VARIABLE_TYPE_BOOL ? existing : -1;
    const int count = names._size;
    const int index = data->AddBoolVariable(RE::hkStringPtr(name), false);
    if (index != count || names._size != count + 1 || data->variableInfos._size != count + 1) return -1;
    // Optional word min/max arrays are empty in the verified chicken graph.
    // CommonLib's AddBoolVariable owns the names/info/initial-value append.
    return index;
}

RE::hkbNode* CopyActivityFlag(RE::hkbNode* source, int originalVariable, int variable, const char* name) {
    if (variable < 0 || !SameVTable(source, RE::VTABLE_BSIsActiveModifier)) return nullptr;
    auto* old = source->variableBindingSet.get();
    if (!old || !SameVTable(old, RE::VTABLE_hkbVariableBindingSet) || old->bindings._size < 1 ||
        old->bindings._size > 8 || !old->bindings._data) {
        logger::error("[PairRig] '{}' source binding mismatch: present={} expectedVariable={}", name, old != nullptr,
            originalVariable);
        return nullptr;
    }
    const RE::hkbVariableBindingSet::Binding* selected = nullptr;
    for (const auto& binding : old->bindings)
        if (binding.variableIndex == originalVariable) {
            if (selected) return nullptr;
            selected = &binding;
        }
    if (!selected || old->indexOfActivationBinding >= 0) return nullptr;
    // A same-graph binding preserves the engine's member-path representation.
    // CommonLib provides the binding-set layout; its GetClassType also returns
    // null. Only the variable index changes; the original set remains intact.
    auto* node = CloneActivityModifier(source, name);
    if (!node) {
        logger::error("[PairRig] '{}' native activity clone failed", name);
        return nullptr;
    }
    auto* bindings = static_cast<RE::hkbVariableBindingSet*>(AllocZero(sizeof(RE::hkbVariableBindingSet)));
    auto* item =
        static_cast<RE::hkbVariableBindingSet::Binding*>(AllocZero(sizeof(RE::hkbVariableBindingSet::Binding)));
    if (!node || !bindings || !item) return nullptr;
    std::memcpy(bindings, old, sizeof(*bindings));
    InitRefObj(bindings, RE::VTABLE_hkbVariableBindingSet);
    *item = *selected;
    item->variableIndex = variable;
    bindings->indexOfActivationBinding = -1;
    OwnArray(bindings->bindings, item, 1);
    node->variableBindingSet.reset(bindings);
    logger::info("[PairRig] '{}' native activity clone prepared: variable {} -> {} bindingDetached={} sourceClone={}",
        name, originalVariable, variable, node->variableBindingSet.get() != old,
        static_cast<int>(source->cloneState.get()));
    return node;
}

// The native anchor of an injection into a creature's root (a pair target's role, the creature's own action, 11-05):
// the profile's root machine and its start state, the activity modifier and the wrapper a new state copies. A profile
// routed beneath native physics (the chicken) injects into its inner behavior machine. nullptr when the graph
// matches the profile, else why not.
struct CreatureAnchor {
    RE::hkbStateMachine* sm = nullptr;
    StateInfoLayout* start = nullptr;  // the root machine's start state
    RE::hkbNode* activity = nullptr;
    RE::hkbNode* wrapperSource = nullptr;
    int returnState = 0;
    TransitionInfoArrayLayout* wild = nullptr;
};
const char* ResolveCreatureAnchor(
    RE::hkbStateMachine* sm, const PairedRig::Profile& profile, NativeNodeSearch& search, CreatureAnchor& out) {
    if (!SameVTable(sm, RE::VTABLE_hkbStateMachine) || !PairedRig::Equal(NodeName(sm), profile.root) ||
        sm->startStateID != profile.startId)
        return "native root signature changed";
    for (auto* item : sm->states) {
        auto* state = reinterpret_cast<StateInfoLayout*>(item);
        if (state && state->stateId == profile.startId && state->name.c_str() &&
            PairedRig::Equal(state->name.c_str(), profile.startName))
            out.start = state;
    }
    if (!out.start || !out.start->generator) return "native default state missing";
    out.activity = search.FindNative(sm, profile.activity);
    if (!SameVTable(out.activity, RE::VTABLE_BSIsActiveModifier)) return "native activity modifier missing";
    out.wrapperSource = search.FindNative(out.start->generator, profile.wrapper);
    // Witchlight has no root physics wrapper. Its recoil wrapper is used only
    // as an allocation template; its recoil modifiers are never retained.
    if (!out.wrapperSource && !profile.preserveModifiers) out.wrapperSource = search.FindNative(sm, profile.wrapper);
    if (!SameVTable(out.wrapperSource, RE::VTABLE_hkbModifierGenerator)) return "native wrapper unavailable";
    out.returnState = profile.startId;
    if (profile.innerRoute) {
        // Preserve the already verified chicken route beneath native physics.
        auto* inner = search.FindNative(out.start->generator, "RootBehavior");
        if (!SameVTable(inner, RE::VTABLE_hkbStateMachine)) return "inner behavior missing";
        sm = static_cast<RE::hkbStateMachine*>(inner);
        out.returnState = 0;
        bool found = false;
        for (auto* item : sm->states) {
            auto* state = reinterpret_cast<StateInfoLayout*>(item);
            if (state && state->stateId == 0 && state->name.c_str() &&
                PairedRig::Equal(state->name.c_str(), "DefaultState"))
                found = true;
        }
        if (!found || sm->startStateID != 0) return "inner default state changed";
    }
    out.sm = sm;
    out.wild = reinterpret_cast<TransitionInfoArrayLayout*>(sm->wildcardTransitions.get());
    if (!out.wild) return "native wildcard transitions missing";
    return nullptr;
}
// A copy of the anchor's native wrapper holding child under modifier; nullptr when either is missing or the copy
// lacks the fields.
RE::hkbGenerator* WrapCreature(
    const CreatureAnchor& anchor, RE::hkbGenerator* child, RE::hkbNode* modifier, const std::string& name) {
    if (!child || !modifier) return nullptr;
    auto* wrapper = CopyPairedWrapper(anchor.wrapperSource, name.c_str());
    auto** gen = ReflectedPointer<RE::hkbGenerator*>(wrapper, "generator");
    auto** mod = ReflectedPointer<RE::hkbNode*>(wrapper, "modifier");
    if (!gen || !mod) return nullptr;
    *gen = child;
    AddRef(child);
    *mod = modifier;
    AddRef(modifier);
    return reinterpret_cast<RE::hkbGenerator*>(wrapper);
}
// The wrappers above every creature state: animation driven (not beneath native physics, whose route drives it
// already) and the profile's native physics modifier. nullptr when one is unavailable.
RE::hkbGenerator* WrapCreatureNative(const CreatureAnchor& anchor, const PairedRig::Profile& profile,
    RE::hkbGenerator* outer, int driven, const std::string& tag) {
    if (profile.innerRoute) return outer;
    outer = WrapCreature(anchor, outer,
        CopyActivityFlag(anchor.activity, driven, driven, (tag + "DrivenActive").c_str()), tag + "DrivenWrapper");
    if (!outer || !profile.preserveModifiers) return outer;
    auto** nativeModifier = ReflectedPointer<RE::hkbNode*>(anchor.wrapperSource, "modifier");
    return nativeModifier ? WrapCreature(anchor, outer, *nativeModifier, tag + "Physics") : nullptr;
}
// A state added to the anchor's machine: the graph rebinds its bindables.
void InvalidateCreatureBindables(RE::hkbBehaviorGraph* graph, const CreatureAnchor& anchor) {
    anchor.sm->areBindablesCached = false;
    graph->areBindablesCached = false;
    if (graph->rootGenerator) graph->rootGenerator->areBindablesCached = false;
    anchor.start->generator->areBindablesCached = false;
}
// A creature's own action (11-05): one clip state in the creature's root machine, entered from any state by its
// enter event (and by the enter events of transitions into it) and left to the start state on its exit events.
// Wrapped as a pair target is (animation driven, the profile's native physics), without the synchronized flags.
void InjectCreatureAction(int action, RE::hkbBehaviorGraph* graph, RE::hkbStateMachine* sm, RE::hkbCharacter* character,
    const PairedRig::Profile& profile, NativeNodeSearch& search) {
    const auto& spec = Actions::Spec(action);
    auto fail = [&](const char* reason) {
        logger::error("[CreatureAction] '{}' {} injection skipped: {}", spec.id, profile.name, reason);
    };
    if (spec.clips.size() != 1) return;
    LoadingProgress::Registration loading(LoadingProgress::Outcome::kFailed);
    CreatureAnchor anchor;
    if (const char* why = ResolveCreatureAnchor(sm, profile, search, anchor)) return fail(why);
    sm = anchor.sm;
    auto& ids = PassIds(sm);
    const int driven = FindName(graph->data->stringData->variableNames, "bAnimationDriven");
    if (driven < 0) return fail("bAnimationDriven unavailable");
    const auto& c = spec.clips[0];
    const int stateId = TakeStateIds(sm, ids, 1);
    if (stateId < 0) return fail("state IDs exhausted");
    const int enter = InjectEvent(graph, c.enterEvent.c_str());
    const int exitEvent = InjectEvent(graph, spec.events.exit.c_str());
    const int recover = spec.HasRecovery() ? InjectEvent(graph, spec.recovery.recoverEvent.c_str()) : -1;
    const int index = EnsureAnimationName(character, c.file.c_str());
    if (enter < 0 || exitEvent < 0 || (spec.HasRecovery() && recover < 0) || index < 0 ||
        index > AnimationLimit::MaxBindingIndex())
        return fail("event or clip registration failed");
    const auto& effects = PassEffects(sm);
    const float enterBlend = spec.enterBlend >= 0 ? spec.enterBlend : 0.2f;
    const float exitBlend = spec.exitBlend >= 0 ? spec.exitBlend : 0.2f;
    auto* enterEffect = SharedEffect(effects.enter, enterBlend, -1);
    auto* exitEffect = SharedEffect(effects.exit, exitBlend, -1, true);
    if (!enterEffect || !exitEffect) return fail("native blend template unavailable");
    std::vector<TriggerDef> triggers;
    if (spec.exitTriggerBeforeEnd >= 0) triggers.push_back({spec.exitTriggerBeforeEnd, exitEvent, true});
    auto* exits = MakeTransitionArray(0);
    auto addExit = [&](int event, RE::hkReferencedObject* effect) {
        TransitionInfo transition{};
        FillTransition(transition, event, anchor.returnState, effect, TransitionInfo::kDisableCondition);
        exits->transitions.push_back(transition);
    };
    addExit(exitEvent, exitEffect);
    for (const auto& w : spec.cancels) {
        const int open = InjectEvent(graph, w.openEvent.c_str());
        const int close = InjectEvent(graph, w.closeEvent.c_str());
        if (open < 0 || close < 0) return fail("window unavailable");
        if (w.openAt >= 0) triggers.push_back({w.openAt, open, false});
        if (w.closeAt >= 0) triggers.push_back({w.closeAt, close, true});
        if (!w.vanillaMatches.empty()) {
            const int cancel = InjectEvent(graph, w.nativeCancelEvent.c_str());
            auto* effect = SharedEffect(effects.exit, w.blend >= 0 ? w.blend : exitBlend, -1);
            if (cancel < 0 || !effect) return fail("cancel window unavailable");
            addExit(cancel, effect);
        }
    }
    if (spec.HasRecovery()) {
        InjectEvent(graph, spec.recovery.event.c_str());
        auto* effect = SharedEffect(effects.exit, spec.recovery.blend >= 0 ? spec.recovery.blend : exitBlend, -1);
        if (!effect) return fail("recovery effect unavailable");
        addExit(recover, effect);
    }
    auto* clip = MakeClip(c.clipName.c_str(), c.file.c_str(), static_cast<std::uint16_t>(index),
        MakeTriggerArray(triggers), nullptr, FrameworkSettings::Get().convertAnnotations);
    if (!clip) return fail("clip allocation failed");
    if (spec.loop.enabled) clip->mode = RE::hkbClipGenerator::kModeLooping;
    auto* outer = WrapCreatureNative(anchor, profile, clip, driven, spec.id + "_CreatureAction");
    if (!outer) return fail("wrapper fields unavailable");
    // Its own enter event, and the enter events of transitions into it (a transition into itself restarts it).
    struct Entry {
        int event;
        RE::hkReferencedObject* effect;
        bool self;
    };
    std::vector<Entry> entries{{enter, enterEffect, false}};
    for (const auto& [from, linkIndex] : Actions::IncomingLinks(action)) {
        const auto& src = Actions::Spec(from);
        if (src.paired.enabled || src.rig != spec.rig || src.clips.size() != 1) continue;
        const auto& link = src.links[linkIndex];
        const int event = InjectEvent(graph, link.enterEvent.c_str());
        const float seconds = link.blend >= 0 ? link.blend : enterBlend;
        auto* effect = SharedLinkEffect(effects.enter, seconds, 1);
        if (event < 0 || !effect) return fail("transition entry unavailable");
        entries.push_back({event, effect, from == action});
    }
    auto* state = MakeState(("AI_" + spec.id + "_CreatureState").c_str(), stateId, outer, exits);
    sm->states.push_back(reinterpret_cast<RE::hkbStateMachine::StateInfo*>(state));
    for (const auto& entry : entries) {
        TransitionInfo transition{};
        FillTransition(transition, entry.event, stateId, entry.effect,
            static_cast<std::int16_t>(TransitionInfo::kDisableCondition | TransitionInfo::kIsLocalWildcard |
                                      (entry.self ? TransitionInfo::kAllowSelfTransitionByTransitionFromAnyState : 0)));
        anchor.wild->transitions.push_back(transition);
    }
    if (exitBlend > 0) ExitBlendHooks::RegisterSharedExit(exitEffect);
    ids.insert(stateId);
    InvalidateCreatureBindables(graph, anchor);
    Result result;
    result.actionIdx = action;
    result.graph = NodeName(sm);
    result.pairedRig = profile.kind;
    result.injected = true;
    result.enterIds = {enter};
    result.stateIds = {stateId};
    result.exitId = exitEvent;
    result.recoverId = recover;
    Publish(std::move(result));
    loading.Ready();
    logger::info("[CreatureAction] '{}' rig={} injected: state={} file='{}' index={} loop={} transitionsIn={}", spec.id,
        profile.name, stateId, c.file, index, spec.loop.enabled, entries.size() - 1);
}
// A pair role in a creature's root state machine: the partner (role 0: "2_", lead) or, since 11-05 phase 2, the
// initiator (role 1: a prefix-less support, read as "NPC"). Both roles read the one paired HKX from the
// character's folder (PairedRig::ClipPath).
void InjectCreaturePair(int action, int role, RE::hkbBehaviorGraph* graph, RE::hkbStateMachine* sm,
    RE::hkbCharacter* character, const PairedRig::Profile& profile, NativeNodeSearch& search) {
    const auto& spec = Actions::Spec(action);
    const char* roleName = role == 0 ? "target" : "initiator";
    auto fail = [&](const char* reason) {
        logger::error("[PairRig] '{}' {} {} injection skipped: {}", spec.id, profile.name, roleName, reason);
    };
    if (spec.clips.size() != 2 || (role == 0 ? spec.paired.targetRig : spec.rig) != profile.kind) return;
    const std::string tag = spec.id + (role == 0 ? "_Creature" : "_CreatureInit");
    LoadingProgress::Registration loading(LoadingProgress::Outcome::kFailed);
    CreatureAnchor anchor;
    if (const char* why = ResolveCreatureAnchor(sm, profile, search, anchor)) return fail(why);
    sm = anchor.sm;
    const int returnState = anchor.returnState;
    auto& ids = PassIds(sm);
    const int driven = FindName(graph->data->stringData->variableNames, "bAnimationDriven");
    const int synced = EnsurePairFlag(graph, "bIsSynced"), pitch = EnsurePairFlag(graph, "DisablePitch");
    if (driven < 0 || synced < 0 || pitch < 0) return fail("activity variables unavailable");
    auto* syncFlag = CopyActivityFlag(anchor.activity, driven, synced, (tag + "SyncActive").c_str());
    auto* pitchFlag = CopyActivityFlag(anchor.activity, driven, pitch, (tag + "PitchActive").c_str());
    if (!syncFlag || !pitchFlag) {
        LogReflectedFields(anchor.activity);
        return fail("activity binding validation failed");
    }
    auto variant = spec.clips[role];
    variant.file = PairedRig::ClipPath(variant.file, profile.kind);
    const int stateId = TakeStateIds(sm, ids, 1);
    if (stateId < 0) return fail("state IDs exhausted");
    const int enter = InjectEvent(graph, variant.enterEvent.c_str());
    const int end = InjectEvent(graph, spec.events.exit.c_str());
    const int index = EnsureAnimationName(character, variant.file.c_str());
    if (enter < 0 || end < 0 || index < 0 || index > AnimationLimit::MaxBindingIndex())
        return fail("event or clip registration failed");
    std::vector<TriggerDef> triggers;
    const float exitAt = spec.exitTriggerBeforeEnd;
    if (exitAt >= 0) triggers.push_back({exitAt, end, true});
    auto* clip = MakeClip(variant.clipName.c_str(), variant.file.c_str(), static_cast<std::uint16_t>(index),
        MakeTriggerArray(triggers), nullptr, true);
    auto* sync = static_cast<RE::BSSynchronizedClipGenerator*>(
        MakeSyncClip(variant.enterEvent.c_str(), clip, role == 0 ? "2_" : "", role == 0, spec.paired.getToMarkTime,
            spec.paired.markErrorThreshold, spec.paired.reorientSupport, spec.paired.applyMotionFromRoot, nullptr));
    if (!sync) return fail("sync allocation failed");
    auto* flagged = WrapCreature(
        anchor, WrapCreature(anchor, sync, syncFlag, tag + "SyncWrapper"), pitchFlag, tag + "PitchWrapper");
    auto* outer = WrapCreatureNative(anchor, profile, flagged, driven, tag);
    if (!outer) return fail("wrapper fields unavailable");
    const auto& effects = PassEffects(sm);
    auto* entryEffect = SharedEffect(effects.enter, std::max(0.0f, spec.enterBlend), -1);
    const float blend = spec.exitBlend >= 0 ? spec.exitBlend : 0.2f;
    // Its own exit effect: ExitBlendHooks finds the pair by this name and records its destination events.
    auto* exitEffect = MakeBlendEffect(effects.exit, (tag + "Exit").c_str(), blend, -1);
    if (!entryEffect || !exitEffect) return fail("native blend template unavailable");
    auto* exits = MakeTransitionArray(0);
    for (const char* name : {spec.events.exit.c_str(), "PairEnd", "2_PairEnd", "pairedStop", "NPCpairedStop"}) {
        const int event = InjectEvent(graph, name);
        if (event < 0) return fail("exit event unavailable");
        TransitionInfo transition{};
        FillTransition(transition, event, returnState, exitEffect, TransitionInfo::kDisableCondition);
        exits->transitions.push_back(transition);
    }
    if (spec.paired.lossBlend > 0) {
        const int event = InjectEvent(graph, spec.paired.lossExitEvent.c_str());
        auto* effect = SharedEffect(effects.exit, spec.paired.lossBlend, -1);
        if (event < 0 || !effect) return fail("loss exit unavailable");
        TransitionInfo transition{};
        FillTransition(transition, event, returnState, effect, TransitionInfo::kDisableCondition,
            FrameworkSettings::Get().wildcardPriority);
        exits->transitions.push_back(transition);
    }
    if (spec.recovery.enabled) {
        const int event = InjectEvent(graph, spec.recovery.recoverEvent.c_str());
        const float seconds = spec.recovery.blend >= 0 ? spec.recovery.blend : blend;
        auto* effect = SharedEffect(effects.exit, seconds, -1);
        if (event < 0 || !effect) return fail("recovery exit unavailable");
        TransitionInfo transition{};
        FillTransition(transition, event, returnState, effect, TransitionInfo::kDisableCondition,
            FrameworkSettings::Get().wildcardPriority);
        exits->transitions.push_back(transition);
    }
    auto* state = MakeState(variant.enterEvent.c_str(), stateId, outer, exits);
    TransitionInfo entry{};
    FillTransition(
        entry, enter, stateId, entryEffect, TransitionInfo::kDisableCondition | TransitionInfo::kIsLocalWildcard);
    sm->states.push_back(reinterpret_cast<RE::hkbStateMachine::StateInfo*>(state));
    anchor.wild->transitions.push_back(entry);
    ExitBlendHooks::Register(exitEffect, blend, action);
    ids.insert(stateId);
    InvalidateCreatureBindables(graph, anchor);
    Result result;
    result.actionIdx = action;
    result.graph = NodeName(sm);
    result.pairedRig = profile.kind;
    result.injected = true;
    result.enterIds = role == 0 ? std::vector<int>{enter, -1} : std::vector<int>{-1, enter};
    result.stateIds = {stateId};
    result.exitId = end;
    Publish(std::move(result));
    loading.Ready();
    PublishSync({sync, clip, variant.file, action, role, false, profile.kind});
    logger::info(
        "[PairRig] '{}' rig={} {} injected: state={} prefix={} file='{}' index={} flags={}/{}; native ragdoll routes retained",
        spec.id, profile.name, roleName, stateId, role == 0 ? "2_" : "NPC", variant.file, index, synced, pitch);
}

// ── Event registration (0.12.209) ────────────────────────────────────────────────────────────────
// Every declared event used to go into every graph of every project (thousands of names into ~20
// graphs of the character project, and into furniture and magic projects that never get an action).
// A project now gets them in its master graph (project event lookups read it, EventMaps) and in each
// graph an action is injected into, right before that injection. Event ids are per graph, so a
// graph's ids do not depend on the others.
struct EventCounts {
    std::size_t added = 0, existing = 0, failed = 0;
};
thread_local std::unordered_set<const void*>* g_eventGraphs = nullptr;  // graphs registered in this symbol pass
thread_local EventCounts* g_eventCounts = nullptr;
void RegisterEvents(RE::hkbBehaviorGraph* graph) {
    if (!graph || !g_eventGraphs || !g_eventGraphs->insert(graph).second) return;
    const auto& names = Actions::AllEventNames();
    auto* data = graph->data.get();
    auto* strings = data ? data->stringData.get() : nullptr;
    if (!strings) {
        if (g_eventCounts) g_eventCounts->failed += names.size();
        return;
    }
    NameLookupScope lookup(strings->eventNames);
    for (const auto& name : names) {
        const int before = strings->eventNames._size;
        const int id = InjectEvent(graph, name.c_str());
        if (!g_eventCounts) continue;
        if (id < 0)
            ++g_eventCounts->failed;
        else if (strings->eventNames._size > before)
            ++g_eventCounts->added;
        else
            ++g_eventCounts->existing;
    }
}

// ── Shared full-body entry (0.12.209, core/FullBodyEntry.h) ─────────────────────────────────────
using Binding = RE::hkbVariableBindingSet::Binding;
const char* BindingPath(const Binding& binding) {
    // hkbVariableBindingSet::Binding starts with the member path (hkStringPtr).
    return reinterpret_cast<const RE::hkStringPtr*>(&binding.bindingPath)->c_str();
}
// A variable binding of `memberPath` in `set`, or nullptr. Variable bindings only (not character
// properties).
const Binding* FindBinding(const RE::hkbVariableBindingSet* set, const char* memberPath) {
    if (!set || !SameVTable(set, RE::VTABLE_hkbVariableBindingSet) || !set->bindings._data) return nullptr;
    for (const auto& binding : set->bindings) {
        const char* path = BindingPath(binding);
        if (path && std::strcmp(path, memberPath) == 0 && static_cast<std::int8_t>(binding.bindingMode) == 0)
            return &binding;
    }
    return nullptr;
}
// Native templates for the two new bindings, searched once under the graph's root: a state
// machine's startStateId binding and a blending transition effect's duration binding (on any
// transition of the machines found there). Copies keep the engine's member path and class.
struct NativeBindings {
    const Binding* startState = nullptr;
    const Binding* duration = nullptr;
    // A machine with one state below iStateIdBase and the engine's state map (core/PairEntry.h).
    const RE::hkbStateMachine* singleState = nullptr;
};
NativeBindings FindNativeBindings(RE::hkbNode* root) {
    NativeBindings found;
    std::vector<RE::hkbNode*> stack{root};
    std::unordered_set<RE::hkbNode*> seen;
    std::vector<HkChild> children;
    auto scanTransitions = [&](const void* list) {
        const auto* array = reinterpret_cast<const TransitionInfoArrayLayout*>(list);
        if (!array || found.duration) return;
        for (int i = 0; i < array->transitions._size && !found.duration; ++i) {
            auto* effect = array->transitions._data[i].transition;
            if (effect && SameVTable(effect, RE::VTABLE_hkbBlendingTransitionEffect))
                found.duration =
                    FindBinding(reinterpret_cast<RE::hkbBindable*>(effect)->variableBindingSet.get(), "duration");
        }
    };
    while (!stack.empty() && seen.size() < 8192 && !(found.startState && found.duration && found.singleState)) {
        auto* node = stack.back();
        stack.pop_back();
        if (!node || !seen.insert(node).second) continue;
        if (SameVTable(node, RE::VTABLE_hkbStateMachine)) {
            auto* machine = static_cast<RE::hkbStateMachine*>(node);
            if (!found.startState) found.startState = FindBinding(machine->variableBindingSet.get(), "startStateId");
            if (!found.singleState && machine->states._size == 1 && machine->stateIDToIndexMap.get())
                if (const auto* only = reinterpret_cast<const StateInfoLayout*>(machine->states._data[0]);
                    only && only->stateId >= 0 && only->stateId < FrameworkSettings::Get().stateIdBase)
                    found.singleState = machine;
            scanTransitions(machine->wildcardTransitions.get());
            for (int k = 0; k < machine->states._size; ++k)
                if (auto* state = reinterpret_cast<StateInfoLayout*>(machine->states._data[k]))
                    scanTransitions(state->transitions);
        }
        const int count = ReadNodeChildren{}(node, children);
        for (int i = count - 1; i >= 0; --i) stack.push_back(children[i].node);
    }
    return found;
}
// A new binding set: `native`'s bindings (if any) followed by `add` retargeted to `variable`. The
// native set is left as it is.
RE::hkbVariableBindingSet* WithBinding(const RE::hkbVariableBindingSet* native, const Binding& add, int variable) {
    auto* set = static_cast<RE::hkbVariableBindingSet*>(AllocZero(sizeof(RE::hkbVariableBindingSet)));
    const int n = native ? native->bindings._size : 0;
    auto* items = static_cast<Binding*>(AllocZero(sizeof(Binding) * static_cast<std::size_t>(n + 1)));
    if (!set || !items) return nullptr;
    if (native)
        std::memcpy(set, native, sizeof(*set));
    else
        set->indexOfActivationBinding = -1;
    InitRefObj(set, RE::VTABLE_hkbVariableBindingSet);
    for (int i = 0; i < n; ++i) items[i] = native->bindings._data[i];
    items[n] = add;
    items[n].variableIndex = variable;
    OwnArray(set->bindings, items, n + 1);
    return set;
}
// The graph's variable `name` of `type`, added (default `value`) when missing; -1 on a mismatch.
template<class Add>
int EnsureVariable(RE::hkbBehaviorGraph* graph, const char* name, RE::hkbVariableInfo::VariableType type, Add add) {
    auto* data = graph ? graph->data.get() : nullptr;
    if (!data || !data->stringData || !data->variableInitialValues) return -1;
    auto& names = data->stringData->variableNames;
    if (names._size != data->variableInfos._size) return -1;
    if (const int existing = FindName(names, name); existing >= 0)
        return data->variableInfos._data[existing].m_type == type ? existing : -1;
    const int count = names._size;
    const int index = add(*data);
    return index == count && names._size == count + 1 && data->variableInfos._size == count + 1 ? index : -1;
}
// Builds the two shared root entries of a character graph (see core/FullBodyEntry.h). On any
// mismatch the reason is returned and the graph gets no full-body action.
const char* PrepareSharedEntry(
    RE::hkbBehaviorGraph* graph, RE::hkbStateMachine* root, TransitionInfoArrayLayout* wild, Pass::FullBody& body) {
    constexpr std::size_t kDuration = 0x50;  // hkbBlendingTransitionEffect::duration (see MakeBlendEffect)
    auto* nativeEffect = body.entry.transition;
    if (!nativeEffect || !SameVTable(nativeEffect, RE::VTABLE_hkbBlendingTransitionEffect))
        return "native entry blend is not a blending transition";
    body.nativeBlend = *reinterpret_cast<const float*>(reinterpret_cast<const std::uint8_t*>(nativeEffect) + kDuration);
    const auto templates = FindNativeBindings(root);
    if (!templates.startState || !templates.duration) return "no native startStateId/duration binding to copy";
    const auto* machine = body.machine;
    const int state = EnsureVariable(graph, FullBodyEntry::kStateVariable,
        RE::hkbVariableInfo::VariableType::VARIABLE_TYPE_INT32, [&](RE::hkbBehaviorGraphData& data) {
            return data.AddIntVariable(RE::hkStringPtr(FullBodyEntry::kStateVariable), machine->startStateID);
        });
    const int blend = EnsureVariable(graph, FullBodyEntry::kBlendVariable,
        RE::hkbVariableInfo::VariableType::VARIABLE_TYPE_REAL, [&](RE::hkbBehaviorGraphData& data) {
            return data.AddFloatVariable(RE::hkStringPtr(FullBodyEntry::kBlendVariable), body.nativeBlend);
        });
    if (state < 0 || blend < 0) return "shared entry variables unavailable";
    const int enter = InjectEvent(graph, FullBodyEntry::kEnterEvent);
    const int linkEnter = InjectEvent(graph, FullBodyEntry::kLinkEnterEvent);
    if (enter < 0 || linkEnter < 0) return "shared entry events unavailable";
    auto* enterEffect = MakeBlendEffect(nativeEffect, "AI_FullBodyEnterBlend", body.nativeBlend, -1);
    auto* linkEffect = MakeBlendEffect(nativeEffect, "AI_FullBodyLinkEnterBlend", body.nativeBlend, 1);
    auto* enterBindings = WithBinding(nullptr, *templates.duration, blend);
    auto* linkBindings = WithBinding(nullptr, *templates.duration, blend);
    auto* startBindings = WithBinding(machine->variableBindingSet.get(), *templates.startState, state);
    if (!enterEffect || !linkEffect || !enterBindings || !linkBindings || !startBindings)
        return "shared entry nodes unavailable";
    reinterpret_cast<RE::hkbBindable*>(enterEffect)->variableBindingSet.reset(enterBindings);
    reinterpret_cast<RE::hkbBindable*>(linkEffect)->variableBindingSet.reset(linkBindings);
    // The native set stays referenced: the machine no longer points at it, but nothing may free it.
    if (auto* native = machine->variableBindingSet.get()) AddRef(native);
    body.machine->variableBindingSet.reset(startBindings);
    body.machine->areBindablesCached = false;
    for (const auto& [event, effect] : {std::pair{enter, enterEffect}, std::pair{linkEnter, linkEffect}}) {
        auto t = body.entry;
        t.eventId = event;
        t.toNestedStateId = 0;
        t.flags = static_cast<std::int16_t>(t.flags & ~TransitionInfo::kToNestedStateIdIsValid);
        t.transition = effect;
        AddRef(t.transition);
        AddRef(t.condition);
        wild->transitions.push_back(t);
    }
    logger::info(
        "[FullBodyEntry] shared entries ready: variables {}={} {}={} (start {} blend {:.2f}s) events {}={} {}={}; "
        "bindings copied from native startStateId/duration",
        FullBodyEntry::kStateVariable, state, FullBodyEntry::kBlendVariable, blend, machine->startStateID,
        body.nativeBlend, FullBodyEntry::kEnterEvent, enter, FullBodyEntry::kLinkEnterEvent, linkEnter);
    return nullptr;
}

// ── Shared pair containers (core/PairEntry.h) ─────────────────────────────────────────────────
// Builds both role containers of a character graph from the validated native role states
// (VampireFeedStanding_Front_Victim / _Player) and their root entries. On any mismatch the reason is
// returned and the graph gets no pair.
const char* PreparePairContainers(RE::hkbBehaviorGraph* graph, RE::hkbStateMachine* root,
    TransitionInfoArrayLayout* wild, std::unordered_set<std::int32_t>& ids, int startStateId,
    const std::array<StateInfoLayout*, 2>& source, const std::array<TransitionInfo, 2>& entry, Pass::Pairs& pairs) {
    constexpr std::size_t kDuration = 0x50;  // hkbBlendingTransitionEffect::duration (see MakeBlendEffect)
    // The nested machines are copies of the root machine. Every loaded machine has the engine's state
    // map before symbol linking reads it; a machine built without one crashed there (0.12.13). The
    // copies point at the map of a native machine with a single state (one entry, index 0, which
    // every container has) whose id is below iStateIdBase, while the nested ids are taken above it:
    // the engine never finds them in that map, as it never finds the states injected into the root in
    // the root's map. The map is only pointed at, never counted or changed (it may not be a
    // reference-counted object), and injected nodes are never destroyed.
    const auto* cls = root->GetClassType();
    if (!cls || cls->objectSize != sizeof(RE::hkbStateMachine)) return "root machine class size differs";
    const auto templates = FindNativeBindings(root);
    if (!templates.startState || !templates.duration) return "no native startStateId/duration binding to copy";
    if (!templates.singleState) return "no single-state native machine to lend its state map";
    // The native role exit: the role state's transition back to the root's start state on its end event.
    const std::array<const char*, 2> ends{"PairEnd", "pairedStop"};
    for (int r = 0; r < 2; ++r) {
        auto& role = pairs.roles[r];
        const auto* list = reinterpret_cast<const TransitionInfoArrayLayout*>(source[r]->transitions);
        bool found = false;
        for (int k = 0; list && k < list->transitions._size; ++k) {
            const auto& t = list->transitions._data[k];
            if (t.toStateId == startStateId && std::strcmp(EventNameOf(graph, t.eventId), ends[r]) == 0) {
                role.exit = t;
                found = true;
            }
        }
        if (!found || !role.exit.transition ||
            !SameVTable(role.exit.transition, RE::VTABLE_hkbBlendingTransitionEffect))
            return "native role exit missing or not a blending transition";
        role.nativeExit =
            *reinterpret_cast<const float*>(reinterpret_cast<const std::uint8_t*>(role.exit.transition) + kDuration);
    }
    // Both containers' root ids, and the first pair's nested id (the start state's default).
    const int base = TakeStateIds(root, ids, 3);
    if (base < 0) return "state IDs exhausted";
    const float nativeExit = pairs.roles[0].nativeExit;
    const int state = EnsureVariable(graph, PairEntry::kStateVariable,
        RE::hkbVariableInfo::VariableType::VARIABLE_TYPE_INT32, [&](RE::hkbBehaviorGraphData& data) {
            return data.AddIntVariable(RE::hkStringPtr(PairEntry::kStateVariable), base + 2);
        });
    std::array<int, 3> blend{};
    const std::array<const char*, 3> blendNames{
        PairEntry::kExitBlendVariable, PairEntry::kLossBlendVariable, PairEntry::kRecoveryBlendVariable};
    for (int kind = 0; kind < 3; ++kind)
        blend[kind] = EnsureVariable(graph, blendNames[kind], RE::hkbVariableInfo::VariableType::VARIABLE_TYPE_REAL,
            [&](RE::hkbBehaviorGraphData& data) {
                return data.AddFloatVariable(RE::hkStringPtr(blendNames[kind]), nativeExit);
            });
    if (state < 0 || std::any_of(blend.begin(), blend.end(), [](int v) { return v < 0; }))
        return "shared pair variables unavailable";
    const std::array<int, 2> enter{
        InjectEvent(graph, PairEntry::kEnterLeadEvent), InjectEvent(graph, PairEntry::kEnterSupportEvent)};
    if (enter[0] < 0 || enter[1] < 0) return "shared pair events unavailable";
    const std::array<const char*, 2> containers{PairEntry::kLeadContainer, PairEntry::kSupportContainer};
    const std::array<const char*, 2> machines{PairEntry::kLeadMachine, PairEntry::kSupportMachine};
    const std::array<const char*, 3> kinds{"Exit", "Loss", "Recovery"};
    for (int r = 0; r < 2; ++r) {
        auto& role = pairs.roles[r];
        for (int kind = 0; kind < 3; ++kind) {
            auto* effect = MakeBlendEffect(role.exit.transition,
                fmt::format("{}_{}Blend", containers[r], kinds[kind]).c_str(), role.nativeExit, -1);
            auto* bindings = effect ? WithBinding(nullptr, *templates.duration, blend[kind]) : nullptr;
            if (!bindings) return "shared pair effects unavailable";
            reinterpret_cast<RE::hkbBindable*>(effect)->variableBindingSet.reset(bindings);
            role.effects[kind] = effect;
        }
        auto* machine = static_cast<RE::hkbStateMachine*>(AllocZero(sizeof(RE::hkbStateMachine)));
        auto* startBindings = WithBinding(nullptr, *templates.startState, state);
        if (!machine || !startBindings) return "container machine unavailable";
        NoteCreatedNode();
        std::memcpy(machine, root, sizeof(RE::hkbStateMachine));
        // Reference members copied from the root are cleared before any is set (nothing is released).
        auto clear = [](auto& member) { std::memset(&member, 0, sizeof(member)); };
        machine->memSizeAndFlags = 0xFFFF;
        machine->referenceCount = 0x4000;
        clear(machine->variableBindingSet);
        EmptyArray(machine->cachedBindables);
        machine->areBindablesCached = false;
        machine->userData = 0;
        MakeString(machine->name, machines[r]);
        machine->id = 0xFFFF;  // assigned when the modified graph is linked
        machine->cloneState = RE::hkbNode::CloneState::kDefault;
        clear(machine->eventToSendWhenStateOrTransitionChanges);
        machine->eventToSendWhenStateOrTransitionChanges.id = RE::hkbEventBase::kNull;
        clear(machine->startStateChooser);
        machine->startStateID = base + 2;
        machine->returnToPreviousStateEventID = -1;
        machine->randomTransitionEventID = -1;
        machine->transitionToNextHigherStateEventID = -1;
        machine->transitionToNextLowerStateEventID = -1;
        machine->syncVariableIndex = -1;
        machine->currentStateID = base + 2;
        machine->wrapAroundStateID = false;
        machine->startStateMode = RE::hkbStateMachine::StartStateMode::kDefault;
        machine->selfTransitionMode = RE::hkbStateMachine::StateMachineSelfTransitionMode::kNoTransition;
        machine->isActive = false;
        EmptyArray(machine->states);
        clear(machine->wildcardTransitions);
        std::memcpy(
            &machine->stateIDToIndexMap, &templates.singleState->stateIDToIndexMap, sizeof(machine->stateIDToIndexMap));
        EmptyArray(machine->activeTransitions);
        EmptyArray(machine->transitionFlags);
        EmptyArray(machine->wildcardTransitionFlags);
        EmptyArray(machine->delayedTransitions);
        machine->timeInState = 0;
        machine->lastLocalTime = 0;
        machine->previousStateID = base + 2;
        machine->stateOrTransitionChanged = false;
        machine->echoNextUpdate = false;
        machine->variableBindingSet.reset(startBindings);
        role.innerWild = MakeTransitionArray(0);
        machine->wildcardTransitions.reset(reinterpret_cast<RE::hkbStateMachine::TransitionInfoArray*>(role.innerWild));
        role.machine = machine;
        role.exits = MakeTransitionArray(0);
        role.id = base + r;
        role.container = MakeState(containers[r], role.id, reinterpret_cast<RE::hkbGenerator*>(machine), role.exits);
        // The role's notify events, sent when the root enters and leaves the container as they were
        // when it entered and left a paired state.
        role.container->enterNotifyEvents = source[r]->enterNotifyEvents;
        AddRef(role.container->enterNotifyEvents);
        role.container->exitNotifyEvents = source[r]->exitNotifyEvents;
        AddRef(role.container->exitNotifyEvents);
        role.entry = entry[r];
        role.entry.eventId = enter[r];
        role.entry.toStateId = role.id;
        role.entry.toNestedStateId = 0;
        role.entry.flags = static_cast<std::int16_t>(role.entry.flags & ~TransitionInfo::kToNestedStateIdIsValid);
        ExitBlendHooks::RegisterSharedPairExit(role.effects[kPairExit]);
        ExitBlendHooks::RegisterSharedPairLoss(role.effects[kPairLoss]);
    }
    for (int i = 0; i < 3; ++i) ids.insert(base + i);
    pairs.graph = graph;
    pairs.root = root;
    pairs.wild = wild;
    pairs.reserved = base + 2;
    logger::info(
        "[PairEntry] containers prepared: lead={} support={} first nested state {}; variables {}={} {}={} {}={} {}={} "
        "(native exit {:.2f}/{:.2f}s) events {}={} {}={}; state map of '{}'",
        base, base + 1, base + 2, PairEntry::kStateVariable, state, blendNames[0], blend[0], blendNames[1], blend[1],
        blendNames[2], blend[2], pairs.roles[0].nativeExit, pairs.roles[1].nativeExit, PairEntry::kEnterLeadEvent,
        enter[0], PairEntry::kEnterSupportEvent, enter[1], NodeName(templates.singleState));
    return nullptr;
}
// One container exit per event, whose effect reads the blend variable of its kind. The loss and
// recovery exits keep at least the wildcard priority, as their per-pair transitions did.
void StagePairExit(RE::hkbBehaviorGraph* graph, Pass::Pairs::Role& role, TransitionInfo transition, int kind) {
    const auto [found, added] = role.exitKinds.try_emplace(transition.eventId, kind);
    if (!added) {
        if (found->second != kind && found->second >= 0) {
            logger::warn("[PairEntry] event '{}' ends one pair as exit kind {} and another as kind {}; the first stays",
                EventNameOf(graph, transition.eventId), found->second, kind);
            found->second = -1 - found->second;  // reported once; still names the first kind
        }
        return;
    }
    transition.transition = role.effects[kind];
    if (kind != kPairExit)
        transition.priority =
            static_cast<std::int16_t>(std::max<int>(transition.priority, FrameworkSettings::Get().wildcardPriority));
    AddRef(transition.transition);
    AddRef(transition.condition);
    role.exits->transitions.push_back(transition);
}
void CommitPairContainers(Pass::Pairs& pairs) {
    if (!pairs.resolved || pairs.failure || !pairs.root || !pairs.wild) return;
    int added = 0, states = 0, exits = 0;
    for (auto& role : pairs.roles) {
        if (!role.container || role.first < 0) continue;
        role.machine->startStateID = role.first;
        role.machine->currentStateID = role.first;
        role.machine->previousStateID = role.first;
        pairs.root->states.push_back(reinterpret_cast<RE::hkbStateMachine::StateInfo*>(role.container));
        auto t = role.entry;
        AddRef(t.transition);
        AddRef(t.condition);
        pairs.wild->transitions.push_back(t);
        role.machine->areBindablesCached = false;
        ++added;
        states += role.states;
        exits += role.exits->transitions._size;
    }
    if (!added) return;
    pairs.root->areBindablesCached = false;
    pairs.graph->areBindablesCached = false;
    logger::info("[PairEntry] shared entries ready: containers={} paired states={} container exits={} "
                 "(registered entries {})",
        added, states, exits, PairEntry::Count());
}

// FullBody route (0.12.18): the action's states join the native FullBodyPlayerIdles machine and
// enter through its IdleReadElderScroll wildcard, so no new state machine or native activation
// binding is constructed.

void InjectFullBodyAction(int action, RE::hkbBehaviorGraph* graph, RE::hkbStateMachine* root,
    TransitionInfoArrayLayout* wild, RE::hkbCharacter* character) {
    LoadingProgress::Registration loading(LoadingProgress::Outcome::kFailed);
    const auto& spec = Actions::Spec(action);
    Result result;
    result.actionIdx = action;
    result.graph = "FullBodyPlayerIdles_Behavior";
    auto fail = [&](const char* reason) {
        result.reason = reason;
        Publish(result);
        logger::error("[FullBodyRoute] action='{}' skipped: {}", spec.id, reason);
    };
    static thread_local Pass::FullBody unpassed;
    auto& body = g_pass ? g_pass->fullBody : (unpassed = {});
    if (!body.resolved) {
        body.resolved = true;
        body.graph = graph;
        body.failure = [&]() -> const char* {
            auto* parent = NamedState(root, "FullBodyPlayerIdles");
            if (!parent || !SameVTable(parent->generator, RE::VTABLE_hkbStateMachine) || !wild)
                return "native full-body parent missing";
            auto* machine = static_cast<RE::hkbStateMachine*>(parent->generator);
            if (std::strcmp(NodeName(machine), "FullBodyPlayerIdles_Behavior") != 0 || !machine->variableBindingSet ||
                !machine->stateIDToIndexMap.get())
                return "native idle bindings/state map missing";
            auto* nativeIdle = NamedState(machine, "IdleReadElderScrollStart");
            if (!nativeIdle) return "native elder-scroll child missing";
            bool haveEntry = false, haveExit = false;
            for (int i = 0; i < wild->transitions._size; ++i) {
                const auto& t = wild->transitions._data[i];
                if (t.toStateId == parent->stateId && t.toNestedStateId == nativeIdle->stateId &&
                    (t.flags & TransitionInfo::kToNestedStateIdIsValid) &&
                    std::strcmp(EventNameOf(graph, t.eventId), "IdleReadElderScroll") == 0) {
                    body.entry = t;
                    haveEntry = true;
                }
            }
            if (auto* parentExits = reinterpret_cast<TransitionInfoArrayLayout*>(parent->transitions))
                for (int i = 0; i < parentExits->transitions._size; ++i) {
                    const auto& t = parentExits->transitions._data[i];
                    if (std::strcmp(EventNameOf(graph, t.eventId), "IdleStop") == 0) {
                        body.exit = t;
                        body.idleStop = t.eventId;
                        haveExit = true;
                    }
                }
            if (!haveEntry || !haveExit || !body.entry.transition || !body.exit.transition)
                return "native entry/exit transitions missing";
            body.parent = parent;
            body.machine = machine;
            body.nativeIdle = nativeIdle;
            return nullptr;
        }();
        // First person has no shared entry: the hook's state and blend variables are the third-person graph's
        // (FullBodyEntry), whose nested state ids differ from these. Each action keeps its own root wildcard.
        if (!body.failure && !g_currentFirstPerson) body.failure = PrepareSharedEntry(graph, root, wild, body);
    }
    if (body.failure) {
        // Every full-body action of the graph needs the same templates: one error for the graph.
        result.reason = body.failure;
        Publish(result);
        if (!std::exchange(body.failureReported, true))
            logger::error("[FullBodyRoute] full-body actions are not injected into this graph: {}", body.failure);
        return;
    }
    auto* parent = body.parent;
    auto* machine = body.machine;
    auto* nativeIdle = body.nativeIdle;
    const auto entry = body.entry;
    const auto exit = body.exit;
    auto& nestedIds = PassIds(machine);
    const int n = static_cast<int>(spec.clips.size());
    if (!n) return fail("empty clip list");
    const int base = TakeStateIds(machine, nestedIds, n);
    if (base < 0) return fail("state IDs exhausted");
    std::vector<int> binding(n), enterIds(n);
    for (int i = 0; i < n; ++i) {
        const auto& c = spec.clips[i];
        binding[i] = EnsureAnimationName(character, c.file.c_str());
        enterIds[i] = InjectEvent(graph, c.enterEvent.c_str());
        if (binding[i] < 0 || binding[i] > AnimationLimit::MaxBindingIndex() || enterIds[i] < 0)
            return fail("clip or event unavailable");
    }
    const int exitEvent = InjectEvent(graph, spec.events.exit.c_str());
    const int recover = spec.HasRecovery() ? InjectEvent(graph, spec.recovery.recoverEvent.c_str()) : -1;
    if (exitEvent < 0 || (spec.HasRecovery() && recover < 0)) return fail("exit event unavailable");
    // An unset duration keeps the native effect; a set one shares the graph's effect of that duration (SharedEffect).
    auto effect = [&](RE::hkReferencedObject* source, float duration, bool exit = false) {
        return duration < 0 ? source : SharedEffect(source, duration, -1, exit);
    };
    auto* enterEffect = effect(entry.transition, spec.enterBlend);
    auto* exitEffect = effect(exit.transition, spec.exitBlend, true);
    if (!enterEffect || !exitEffect) return fail("effects unavailable");
    if (spec.exitBlend > 0) ExitBlendHooks::RegisterSharedExit(exitEffect);
    struct Window {
        int open, close, cancel;
        RE::hkReferencedObject* effect;
        RE::hkReferencedObject* exitEffect;
    };
    std::vector<Window> windows;
    for (const auto& w : spec.cancels) {
        Window ids{InjectEvent(graph, w.openEvent.c_str()), InjectEvent(graph, w.closeEvent.c_str()),
            w.vanillaMatches.empty() ? -1 : InjectEvent(graph, w.nativeCancelEvent.c_str()),
            effect(entry.transition, w.blend), effect(exit.transition, w.blend)};
        if (ids.open < 0 || ids.close < 0 || !ids.effect || !ids.exitEffect ||
            (!w.vanillaMatches.empty() && ids.cancel < 0))
            return fail("window unavailable");
        windows.push_back(ids);
    }
    std::vector<TransitionInfo> exits;  // this action's parent exits, staged once all are prepared
    auto addExit = [&](int event, int state, RE::hkReferencedObject* eff) {
        auto t = exit;
        t.eventId = event;
        t.transition = eff;
        t.fromNestedStateId = state;
        t.flags = static_cast<std::int16_t>(t.flags | TransitionInfo::kFromNestedStateIdIsValid);
        exits.push_back(t);
    };
    // Everything is prepared before the graph changes: a failure below leaves the native
    // machine, both wildcard arrays and the parent exits untouched.
    struct PendingEntry {
        int event, state;
        RE::hkReferencedObject* effect;
        const std::string* name;  // the enter event, for the shared entry
        float blend;              // seconds the shared entry blends in
        bool link;
        bool self = false;  // a transition of this action into itself: only inside, as a self-transition
    };
    std::vector<PendingEntry> entries;
    std::vector<StateInfoLayout*> states;
    auto addEntry = [&](int event, int state, RE::hkReferencedObject* eff, const std::string& name, float blend,
                        bool link) { entries.push_back({event, state, eff, &name, blend, link}); };
    for (int i = 0; i < n; ++i) {
        const auto& c = spec.clips[i];
        std::vector<TriggerDef> triggers;
        if (spec.exitTriggerBeforeEnd >= 0) triggers.push_back({spec.exitTriggerBeforeEnd, exitEvent, true});
        auto* transitions = MakeTransitionArray(0);
        for (std::size_t w = 0; w < spec.cancels.size(); ++w) {
            const auto& win = spec.cancels[w];
            const auto& ids = windows[w];
            if (win.openAt >= 0) triggers.push_back({win.openAt, ids.open, false});
            if (win.closeAt >= 0) triggers.push_back({win.closeAt, ids.close, true});
            if (ids.cancel >= 0) {
                addExit(ids.cancel, base + i, ids.exitEffect);
                ++result.cancelOutCount;
            }
        }
        auto* clip = MakeClip(c.clipName.c_str(), c.file.c_str(), binding[i], MakeTriggerArray(triggers),
            nativeIdle->generator, FrameworkSettings::Get().convertAnnotations);
        if (spec.loop.enabled) clip->mode = RE::hkbClipGenerator::kModeLooping;  // charge (10-30): plays on
        states.push_back(MakeState(("AI_" + spec.id + "_State" + c.tag).c_str(), base + i, clip, transitions));
        addExit(exitEvent, base + i, exitEffect);
        const auto& recovery = spec.recovery;
        if (recovery.enabled) {
            InjectEvent(graph, recovery.event.c_str());
            auto* recoverEffect = effect(exit.transition, recovery.blend >= 0 ? recovery.blend : spec.exitBlend);
            if (!recoverEffect) return fail("clip recovery effect unavailable");
            addExit(recover, base + i, recoverEffect);
        }
        addEntry(enterIds[i], base + i, enterEffect, c.enterEvent,
            spec.enterBlend >= 0 ? spec.enterBlend : body.nativeBlend, false);
        result.stateIds.push_back(base + i);
    }
    // Direct ordinary links retain their own entry event and blend.
    if (n == 1)
        for (const auto& [from, linkIndex] : Actions::IncomingLinks(action)) {
            const auto& src = Actions::Spec(from);
            if (src.paired.enabled || src.clips.size() != 1) continue;
            const auto& link = src.links[linkIndex];
            const int event = InjectEvent(graph, link.enterEvent.c_str());
            const float seconds = link.blend >= 0 ? link.blend : spec.enterBlend >= 0 ? spec.enterBlend : 0.0f;
            auto* eff = SharedLinkEffect(entry.transition, seconds, 1);
            if (event >= 0 && eff) {
                addEntry(event, base, eff, link.enterEvent, seconds, true);
                entries.back().self = from == action;
                logger::debug(
                    "[FullBodyLink] '{}' -> '{}' event='{}' innerState={}", src.id, spec.id, link.enterEvent, base);
            }
        }
    // Commit: states, entries (outer and inner wildcards) and exits together.
    auto* innerWild = reinterpret_cast<TransitionInfoArrayLayout*>(machine->wildcardTransitions.get());
    if (!innerWild) {
        innerWild = MakeTransitionArray(0);
        machine->wildcardTransitions.reset(reinterpret_cast<RE::hkbStateMachine::TransitionInfoArray*>(innerWild));
    }
    for (auto* state : states) machine->states.push_back(reinterpret_cast<RE::hkbStateMachine::StateInfo*>(state));
    for (auto& t : exits) {
        AddRef(t.transition);
        AddRef(t.condition);
        body.exits.push_back(t);
        // The former per-action scan took the last IdleStop exit of the parent as the next template.
        if (t.eventId == body.idleStop) body.exit = t;
    }
    for (const auto& pending : entries) {
        // A transition of an action into itself is only ever sent inside FullBody: no way in from outside.
        if (!pending.self) {
            if (g_currentFirstPerson) {
                // Outside FullBody in first person, the original event (the hook forwards it) enters the parent
                // and selects this nested destination, as every view did before 0.12.209.
                auto t = entry;
                t.eventId = pending.event;
                t.toNestedStateId = pending.state;
                t.transition = pending.effect;
                AddRef(t.transition);
                AddRef(t.condition);
                wild->transitions.push_back(t);
            } else
                // Outside FullBody, the shared entry (the NotifyAnimationGraph hook sends it for this event)
                // enters the parent and selects this nested destination.
                FullBodyEntry::Register(pending.name->c_str(), pending.state, pending.blend, pending.link);
        }
        // Inside FullBody, change children directly. Re-entering the active
        // parent is a self-transition and may be rejected by the native graph. An action's transition into itself
        // restarts its clip, the effect blending from the pose it leaves (10-31).
        TransitionInfo inner{};
        FillTransition(inner, pending.event, pending.state, pending.effect,
            static_cast<std::int16_t>(
                TransitionInfo::kIsLocalWildcard | TransitionInfo::kDisableCondition |
                (pending.self ? TransitionInfo::kAllowSelfTransitionByTransitionFromAnyState : 0)),
            entry.priority);
        innerWild->transitions.push_back(inner);
    }
    if (!g_pass) CommitFullBodyExits(body);
    machine->areBindablesCached = false;
    root->areBindablesCached = false;
    graph->areBindablesCached = false;
    result.enterIds = std::move(enterIds);
    result.exitId = exitEvent;
    result.recoverId = recover;
    result.injected = true;
    Publish(result);
    loading.Ready();
    logger::info("[FullBodyRoute] action='{}' view={} parent={} machine='{}' states={} first={} entryPriority={} "
                 "nativeCancelWindows={}",
        spec.id, g_currentFirstPerson ? "first" : "third", parent->stateId, NodeName(machine), n, base, entry.priority,
        result.cancelOutCount);
}

// ── 액션 하나를 그래프 하나에 ───────────────────────────────────────────────
void InjectAction(int a_actionIdx, const Target& tgt, RE::hkbBehaviorGraph* a_graph, RE::hkbStateMachine* sm,
    StateInfoLayout* startState, TransitionInfoArrayLayout*& wild, std::unordered_set<std::int32_t>& ids,
    RE::hkbCharacter* a_character, bool a_airRoute = false) {
    LoadingProgress::Registration loading(LoadingProgress::Outcome::kFailed);
    const auto& fw = FrameworkSettings::Get();
    // Only entry events differ. Clip identity and runtime slots stay attached
    // to the same action so normal and airborne paths share one runner.
    auto spec = Actions::Spec(a_actionIdx);
    if (a_airRoute)
        for (auto& clip : spec.clips) clip.enterEvent = clip.airEnterEvent;
    Result res;
    res.actionIdx = a_actionIdx;
    res.graph = tgt.smName;
    res.airRoute = a_airRoute;
    auto fail = [&](std::string a_why) {
        res.reason = std::move(a_why);
        logger::warn("[Inject] {}/{}: 건너뜀 — {}", spec.id, res.graph, res.reason);
        Publish(res);
    };

    // Native standing, moving and falling jump states share this activity
    // modifier. Reuse its same-graph bindings, but not jump entry events
    // (which could restart the jump impulse).
    std::vector<RE::hkbNode*> airWrappers;
    if (a_airRoute) {
        auto* jump = NamedState(sm, "JumpStanding");
        RE::hkbNode* source = jump ? jump->generator : nullptr;
        if (!SameVTable(source, RE::VTABLE_hkbModifierGenerator)) return fail("native jump wrapper missing");
        auto** child = ReflectedPointer<RE::hkbGenerator*>(source, "generator");
        auto** modifier = ReflectedPointer<RE::hkbNode*>(source, "modifier");
        if (!child || !*child || !modifier || !*modifier || !SameVTable(*modifier, RE::VTABLE_BSIsActiveModifier) ||
            std::strcmp(NodeName(*modifier), "Jump_BSIsActiveModifier") != 0 || !(*modifier)->variableBindingSet)
            return fail("native jump activity modifier/bindings mismatch");
        // The wrapper copier uses runtime class metadata and resets node
        // identity/caches. It is also used by the native paired wrappers.
        for (const auto& clip : spec.clips) {
            auto* wrapper = CopyPairedWrapper(source, (clip.airEnterEvent + "_JumpWrapper").c_str());
            auto** copiedChild = ReflectedPointer<RE::hkbGenerator*>(wrapper, "generator");
            auto** copiedModifier = ReflectedPointer<RE::hkbNode*>(wrapper, "modifier");
            if (!copiedChild || !copiedModifier || *copiedChild != *child || *copiedModifier != *modifier)
                return fail("copied jump wrapper links mismatch");
            airWrappers.push_back(wrapper);
        }
        logger::debug(
            "[AirRoute] '{}' native jump modifier='{}' wrappers={}", spec.id, NodeName(*modifier), airWrappers.size());
    }

    // 이벤트 (그래프 로컬 id)
    const int n = static_cast<int>(spec.clips.size());
    for (auto& c : spec.clips) res.enterIds.push_back(InjectEvent(a_graph, c.enterEvent.c_str()));
    res.exitId = InjectEvent(a_graph, spec.events.exit.c_str());
    if (spec.HasRecovery()) {
        InjectEvent(a_graph, spec.recovery.event.c_str());  // 주석이 쏘는 슬롯 이벤트 (등록만)
        res.recoverId = InjectEvent(a_graph, spec.recovery.recoverEvent.c_str());
        if (res.recoverId < 0) return fail("Recover 이벤트 등록 실패");
    }
    for (int i = 0; i < n; ++i)
        if (res.enterIds[i] < 0) return fail("진입 이벤트 등록 실패");
    if (res.exitId < 0) return fail("Exit 이벤트 등록 실패");
    struct WinIds {
        std::int32_t open, close;
    };
    std::vector<WinIds> winIds;
    for (const auto& w : spec.cancels) {
        WinIds ids{InjectEvent(a_graph, w.openEvent.c_str()), InjectEvent(a_graph, w.closeEvent.c_str())};
        if (ids.open < 0 || ids.close < 0) return fail("창 이벤트 등록 실패");
        winIds.push_back(ids);
    }

    // 애니 이름 (이 캐릭터의 문자열 데이터. 다른 캐릭터 파일은 LoadClips 훅이 채운다)
    std::vector<std::uint16_t> bind(spec.clips.size(), 0);
    {
        auto* setup = a_character ? a_character->setup.get() : nullptr;
        auto* cdata = setup ? setup->data.get() : nullptr;
        auto* csd = cdata ? cdata->stringData.get() : nullptr;
        if (!csd) return fail("hkbCharacterStringData 없음");
        for (std::size_t i = 0; i < spec.clips.size(); ++i) {
            int idx = FindName(csd->animationNames, spec.clips[i].file.c_str());
            if (idx < 0) {
                if (!AppendName(csd->animationNames, spec.clips[i].file.c_str()))
                    return fail("animationNames push 실패");
                idx = csd->animationNames._size - 1;
            }
            bind[i] = static_cast<std::uint16_t>(idx);
        }
    }

    // 전환 효과: 기존 것 재사용, 블렌드 길이를 지정했으면 복제해서 만든다 (V4)
    const Effects& eff = PassEffects(sm);
    if (!eff.enter || !eff.exit) return fail("전환 효과를 못 찾음");
    RE::hkReferencedObject* enterEff = eff.enter;
    std::string enterName = eff.enterName;
    RE::hkReferencedObject* exitEff = eff.exit;
    std::string exitName = eff.exitName;
    if (spec.enterBlend >= 0.0f)
        if (auto* e = SharedEffect(eff.enter, spec.enterBlend, 1)) {
            enterEff = e;
            enterName = NodeName(e);
        }
    if (spec.exitBlend >= 0.0f)
        if (auto* e = SharedEffect(eff.exit, spec.exitBlend, 1, true)) {
            exitEff = e;
            exitName = NodeName(e);
            if (spec.exitBlend > 0.0f) ExitBlendHooks::RegisterSharedExit(e);
        }
    // stateId 범위 (겹치지 않게)
    const std::int32_t base = TakeStateIds(sm, ids, n);
    if (base < 0) return fail("state IDs exhausted");
    const auto prio = static_cast<std::int16_t>(std::clamp(fw.wildcardPriority, 0, 32767));

    // 창 구간 적용 (이벤트 기반만 동작 — V9)
    auto applyWindow = [&](TransitionInfo& t, const CancelSpec& w, const WinIds& wi) {
        switch (w.mode) {
        case 1:
            t.flags = static_cast<std::int16_t>(t.flags | TransitionInfo::kUseTriggerInterval);
            t.triggerInterval = {wi.open, wi.close, 0.0f, 0.0f};
            break;
        case 2:
            t.flags = static_cast<std::int16_t>(
                t.flags | TransitionInfo::kUseTriggerInterval | TransitionInfo::kUseInitiateInterval);
            t.triggerInterval = {-1, wi.close, 0.0f, 0.0f};
            t.initiateInterval = {wi.open, wi.close, 0.0f, 0.0f};
            break;
        default: break;
        }
    };

    // 캔슬 아웃(vanilla:*) 상속 후보: 시작 상태의 전이 중 이름에 부분 문자열이 든 것 (창마다)
    std::vector<std::vector<const TransitionInfo*>> inherit(spec.cancels.size());
    std::string inheritedNames;
    if (startState->transitions) {
        auto* sa = reinterpret_cast<TransitionInfoArrayLayout*>(startState->transitions);
        for (std::size_t w = 0; w < spec.cancels.size(); ++w) {
            for (int k = 0; k < sa->transitions._size; ++k) {
                const auto& t = sa->transitions._data[k];
                const char* en = EventNameOf(a_graph, t.eventId);
                bool hit = false;
                for (const auto& m : spec.cancels[w].vanillaMatches)
                    if (Contains(en, m.c_str())) {
                        hit = true;
                        break;
                    }
                if (hit) {
                    inherit[w].push_back(&t);
                    if (w == 0) {
                        inheritedNames += en;
                        inheritedNames += ' ';
                    }
                }
            }
        }
    }

    const RE::hkbNode* tmpl = sm;
    int cancelCount = 0;
    // States are added to the machine only after every clip was prepared (see the commit below).
    std::vector<StateInfoLayout*> newStates;
    for (int i = 0; i < n; ++i) {
        const auto& c = spec.clips[i];
        std::vector<TriggerDef> trig;  // 시각이 음수인 슬롯은 HKX 주석이 이벤트를 쏜다 (트리거 생성 안 함)
        if (spec.exitTriggerBeforeEnd >= 0.0f) trig.push_back({spec.exitTriggerBeforeEnd, res.exitId, true});
        for (std::size_t w = 0; w < spec.cancels.size(); ++w) {
            if (spec.cancels[w].openAt >= 0.0f) trig.push_back({spec.cancels[w].openAt, winIds[w].open, false});
            if (spec.cancels[w].closeAt >= 0.0f) trig.push_back({spec.cancels[w].closeAt, winIds[w].close, true});
        }
        auto* clip =
            MakeClip(c.clipName.c_str(), c.file.c_str(), bind[i], MakeTriggerArray(trig), tmpl, fw.convertAnnotations);
        if (spec.loop.enabled) clip->mode = RE::hkbClipGenerator::kModeLooping;  // charge (10-30): plays on

        // 전이 배열: [Exit] + [Recovery] + [상속 캔슬 아웃]
        const auto& recovery = spec.recovery;
        int total = 1 + (recovery.enabled ? 1 : 0);
        for (std::size_t w = 0; w < spec.cancels.size(); ++w) total += static_cast<int>(inherit[w].size());
        auto* arr = MakeTransitionArray(total);
        int at = 0;
        FillTransition(arr->transitions._data[at++], res.exitId, tgt.startStateId, exitEff, 0);
        if (recovery.enabled) {
            const float seconds = recovery.blend >= 0 ? recovery.blend : spec.exitBlend;
            auto* recoverEff = seconds < 0 ? eff.exit : SharedEffect(eff.exit, seconds, 1);
            if (!recoverEff) return fail("clip recovery effect unavailable");
            FillTransition(arr->transitions._data[at++], res.recoverId, tgt.startStateId, recoverEff, 0, prio);
        }  // Recovery → 이동
        for (std::size_t w = 0; w < spec.cancels.size(); ++w) {
            const auto& win = spec.cancels[w];
            for (const auto* src : inherit[w]) {
                auto& t = arr->transitions._data[at++];
                t = *src;
                t.priority = static_cast<std::int16_t>(
                    std::max<int>(t.priority, prio));  // 진입 블렌드 중 선입력 보관에 필요 (V9)
                applyWindow(t, win, winIds[w]);
                ++cancelCount;
            }
        }
        RE::hkbGenerator* gen = clip;
        if (a_airRoute) {
            auto* wrapper = airWrappers[i];
            auto** child = ReflectedPointer<RE::hkbGenerator*>(wrapper, "generator");
            auto** modifier = ReflectedPointer<RE::hkbNode*>(wrapper, "modifier");
            *child = clip;
            AddRef(clip);
            AddRef(*modifier);
            gen = static_cast<RE::hkbGenerator*>(wrapper);
        }
        if (c.syncRole != 0) {  // 페어: BSSynchronizedClipGenerator 로 감싼다
            const auto& pd = spec.paired;
            // ★생성기 이름 = 진입 이벤트 이름 (바닐라: 상태·생성기 'HugA' ↔ 이벤트 'HugA'). 엔진은 상대 이벤트를 "pa_" 뗀 이름의 sync 생성기로 찾는다
            gen = MakeSyncClip(c.enterEvent.c_str(), clip, c.syncRole == 1 ? "2_" : "", c.syncRole == 1,
                pd.getToMarkTime, pd.markErrorThreshold, pd.reorientSupport, pd.applyMotionFromRoot, tmpl);
            PublishSync({static_cast<RE::BSSynchronizedClipGenerator*>(gen), clip, c.file, a_actionIdx, i,
                g_currentFirstPerson});
        }
        newStates.push_back(MakeState(
            c.syncRole != 0 ? c.enterEvent.c_str() : ("AI_" + spec.id + "_State" + c.tag).c_str(), base + i, gen, arr));
    }
    for (int i = 0; i < n; ++i) {
        sm->states.push_back(reinterpret_cast<RE::hkbStateMachine::StateInfo*>(newStates[i]));
        ids.insert(base + i);
        res.stateIds.push_back(base + i);
    }
    res.cancelOutCount = n > 0 ? cancelCount / n : 0;

    // 진입 와일드카드
    if (!wild) {
        wild = MakeTransitionArray(0);
        *reinterpret_cast<void**>(&sm->wildcardTransitions) = wild;
    }
    for (int i = 0; i < n; ++i) {
        TransitionInfo ti{};
        FillTransition(ti, res.enterIds[i], base + i, enterEff,
            static_cast<std::int16_t>(TransitionInfo::kIsLocalWildcard | TransitionInfo::kDisableCondition), prio);
        wild->transitions.push_back(ti);
    }
    // Each ordinary link has its own event; a link with its own blend gets an immutable effect shared by every link
    // with that blend (SharedLinkEffect). Never mutate the destination's effect while another actor blends through it.
    if (!spec.paired.enabled && n == 1)
        for (const auto& [source, linkIndex] : Actions::IncomingLinks(a_actionIdx)) {
            const auto& from = Actions::Spec(source);
            if (from.paired.enabled || from.clips.size() != 1) continue;
            const auto& link = from.links[linkIndex];
            const auto& name = a_airRoute ? link.airEnterEvent : link.enterEvent;
            const int event = InjectEvent(a_graph, name.c_str());
            if (event < 0) continue;
            auto* effect = enterEff;
            if (link.blend >= 0) {
                effect = SharedLinkEffect(eff.enter, link.blend, 1);
                if (!effect) continue;
            }
            // A transition into itself restarts the clip (10-31).
            TransitionInfo edge{};
            FillTransition(edge, event, base, effect,
                static_cast<std::int16_t>(
                    TransitionInfo::kIsLocalWildcard | TransitionInfo::kDisableCondition |
                    (source == a_actionIdx ? TransitionInfo::kAllowSelfTransitionByTransitionFromAnyState : 0)),
                prio);
            wild->transitions.push_back(edge);
        }
    res.injected = true;
    Publish(res);
    loading.Ready();
    if (a_airRoute) AirControl::RegisterRoute(a_graph, sm, a_actionIdx, res.stateIds);
    if (a_airRoute)
        logger::debug(
            "[AirRoute] '{}' injected in {}: states={} exitState={}", spec.id, tgt.smName, n, tgt.startStateId);
    logger::debug(
        "[Inject] {}/{}: 삽입 완료 — 상태 {}개 (stateId {}..{}), 효과 enter='{}' exit='{}', exit 이벤트 {}, 바닐라 전환 {}개, 캔슬아웃 상속 {}개/상태{}",
        spec.id, res.graph, n, base, base + n - 1, enterName, exitName, res.exitId, spec.cancels.size(),
        res.cancelOutCount, inheritedNames.empty() ? "" : (" [" + inheritedNames + "]"));
}

RE::hkbNode* FindNodeByName(RE::hkbNode* node, const char* name, int depth, std::size_t& seen) {
    NativeNodeSearch search;
    auto* result = search.Find(node, name, depth);
    seen = search.LastVisited();
    logger::debug("[InjectProfile] node search root='{}' query='{}' visited={} bufferGrowths={}", NodeName(node), name,
        search.LastVisited(), search.bufferGrowths);
    return result;
}

// ── 그래프 하나: 자가검증 뒤 모든 액션 삽입 ────────────────────────────────
// A 0_master right before the engine numbers it: the loading panel's graph row, the log, and past the id limit the
// set core/NodeIdRenumber renumbers. Counted once everything Animation Injector adds is in (a test game 11-07: 7010 +
// 506 = 7516 counted before the commit as 7244, the engine 7549).
void ReportNodeIds(RE::hkbBehaviorGraph* a_graph, RE::hkbNode* a_root, const char* a_label, std::size_t a_native,
    std::size_t a_made, bool a_renumbering, bool a_leftOut) {
    const std::size_t counted = CountNodes(a_root);
    logger::info(
        "[NodeBudget] {} 0_master: native {} + AI {} = {}, counted {} node ids of {} ({} kept free for clones)",
        a_label, a_native, a_made, a_native + a_made, counted, kNodeIdLimit, kNodeIdsKeptFree);
    if (counted + kNodeIdsKeptFree <= kNodeIdLimit) {
        LoadingProgress::ReportNodes(counted, kNodeIdLimit, a_leftOut);
        return;
    }
    // Past the id limit core/NodeIdRenumber needs the set the engine is about to number.
    auto nodes = AllNodes(a_root);
    const std::size_t machines = NodeIdRenumber::Machines(nodes);
    NodeIdRenumber::NoteCounted(a_graph, std::move(nodes));
    const std::size_t machineIds = machines + NodeIdRenumber::kCloneIds;
    if (a_renumbering && machineIds < NodeIdRenumber::kMachineIdLimit) {
        logger::info(
            "[NodeBudget] {} 0_master: {} nodes pass the {} node ids; state machines get their own ids when the engine "
            "numbers it ({} machines + {} for clones of {})",
            a_label, counted, kNodeIdLimit, machines, NodeIdRenumber::kCloneIds, NodeIdRenumber::kMachineIdLimit);
        LoadingProgress::ReportNodes(counted, kNodeIdLimit, a_leftOut, machineIds, NodeIdRenumber::kMachineIdLimit);
        return;
    }
    LoadingProgress::ReportNodes(counted, kNodeIdLimit, true);
    logger::error("[NodeBudget] {} 0_master: {} nodes leave fewer than {} of the {} node ids free", a_label, counted,
        kNodeIdsKeptFree, kNodeIdLimit);
}

// No action installed: Animation Injector still shows the character's limits on every load (some install it for the
// panel alone) and keeps a graph past the node ids safe. Nothing is added to any graph.
void MeasureCharacterProject(
    const RE::BSScrapArray<RE::hkbBehaviorGraph*>& a_graphs, RE::hkbCharacter* a_character, bool a_firstPerson) {
    auto* setup = a_character ? a_character->setup.get() : nullptr;
    auto* data = setup ? setup->data.get() : nullptr;
    auto* strings = data ? data->stringData.get() : nullptr;
    const char* project = strings && strings->name.c_str() ? strings->name.c_str() : "";
    if (!a_firstPerson && PairedRig::CharacterName(project) != PairedRig::Kind::kCharacter) return;
    LoadingProgress::Registration loading;  // the panel shows this load, with nothing else to load
    if (strings)
        LoadingProgress::ReportAnimations(static_cast<std::size_t>(std::max(0, strings->animationNames._size)),
            static_cast<std::size_t>(AnimationLimit::Get().bindingCap), false);
    for (auto* graph : a_graphs) {
        auto* root = graph ? graph->rootGenerator.get() : nullptr;
        const char* name = root ? NodeName(root) : nullptr;
        if (!name || _stricmp(name, "Master_Behavior") != 0 || !Claim(graph)) continue;
        // Test mode: as a load order with many behavior mods.
        if (const auto stress = static_cast<std::size_t>(FrameworkSettings::Get().nodeIdStressNodes);
            stress && !a_firstPerson) {
            std::size_t seen = 0;
            if (auto* machine = FindNodeByName(root, "RootBehaviorGraph", 0, seen);
                machine && SameVTable(machine, RE::VTABLE_hkbStateMachine))
                NodeIdStress::Inflate(graph, static_cast<RE::hkbStateMachine*>(machine), stress);
        }
        ReportNodeIds(graph, root, a_firstPerson ? "first-person" : "third-person", CountNodes(root), 0,
            NodeIdRenumber::Active(), false);
    }
}

void TryInjectGraph(RE::hkbBehaviorGraph* a_graph, RE::hkbCharacter* a_character) {
    if (!a_graph || Claimed(a_graph)) return;
    PassScope pass;
    auto* root = a_graph->rootGenerator.get();
    if (!root) return;
    auto* setup = a_character ? a_character->setup.get() : nullptr;
    auto* data = setup ? setup->data.get() : nullptr;
    auto* strings = data ? data->stringData.get() : nullptr;
    const auto rig = strings && strings->name.c_str() ? PairedRig::CharacterName(strings->name.c_str())
                                                      : PairedRig::Kind::kUnsupported;
    if (PairedRig::IsCreature(rig)) {
        const auto* actual = PairedRig::Find(rig);
        if (g_currentFirstPerson || !PairedRig::Equal(NodeName(root), actual->root)) return;
        if (!Claim(a_graph)) return;  // another loader thread took this graph first
        if (!FrameworkSettings::Get().injectState) return;
        if (!SameVTable(root, RE::VTABLE_hkbStateMachine)) return;
        RegisterEvents(a_graph);
        NativeNodeSearch search;
        for (int a = 0; a < Actions::Count(); ++a) {
            const auto& spec = Actions::Spec(a);
            if (spec.clips.empty()) continue;  // a selecting action has nothing to play (XmlV2, 0.12.244)
            if (spec.paired.enabled) {
                // Each role whose rig this graph shares (same folder and root behavior): the partner's and, for a
                // creature-started pair (11-05 phase 2), the initiator's.
                const auto plays = [&](PairedRig::Kind wanted) {
                    return PairedRig::IsCreature(wanted) && PairedRig::SharesAssets(wanted, rig) &&
                           PairedRig::Equal(PairedRig::Find(wanted)->root, actual->root);
                };
                if (plays(spec.paired.targetRig))
                    InjectCreaturePair(a, 0, a_graph, static_cast<RE::hkbStateMachine*>(root), a_character,
                        *PairedRig::Find(spec.paired.targetRig), search);
                if (plays(spec.rig))
                    InjectCreaturePair(a, 1, a_graph, static_cast<RE::hkbStateMachine*>(root), a_character,
                        *PairedRig::Find(spec.rig), search);
            }
            // A creature's own action plays on its whole skeleton family (11-05): every creature sharing its folder
            // and skeleton (dog and wolf, draugr and skeleton), each through its own graph's anchor. An HKX cannot
            // tell those creatures apart, so the editor writes whichever one it matched.
            else if (PairedRig::SharesAssets(spec.rig, rig))
                InjectCreatureAction(a, a_graph, static_cast<RE::hkbStateMachine*>(root), a_character, *actual, search);
        }
        logger::info("[InjectProfile] creature={} visits={} nativeCacheHits={} bufferGrowths={}", actual->name,
            search.visits, search.cacheHits, search.bufferGrowths);
        return;
    }
    if (rig != PairedRig::Kind::kCharacter && !g_currentFirstPerson) return;
    const char* rootName = NodeName(root);
    const Target* tgt = nullptr;
    RE::hkbNode* smNode = root;
    for (const auto& t : kTargets)
        if (_stricmp(rootName, t.smName) == 0) {
            tgt = &t;
            break;
        }
    if (!tgt && rootName && _stricmp(rootName, "Master_Behavior") == 0) {  // 0_master: 루트 SM 은 중첩돼 있다
        std::size_t seen = 0;
        smNode = FindNodeByName(root, "RootBehaviorGraph", 0, seen);
        if (smNode)
            for (const auto& t : kTargets)
                if (_stricmp(t.smName, "RootBehaviorGraph") == 0) tgt = &t;
        logger::debug("[Inject] 0_master: RootBehaviorGraph {} (방문 {}개)", smNode ? "찾음" : "못 찾음", seen);
    }
    if (!tgt || !smNode) return;
    if (!Claim(a_graph)) return;  // another loader thread took this graph first

    auto skip = [&](const std::string& a_why) { logger::warn("[Inject] {}: 건너뜀 — {}", tgt->smName, a_why); };
    if (!SameVTable(smNode, RE::VTABLE_hkbStateMachine)) return skip("(a) 대상이 hkbStateMachine 이 아님");
    auto* sm = static_cast<RE::hkbStateMachine*>(smNode);
    if (sm->startStateID != tgt->startStateId)
        return skip(fmt::format("(a) startStateId {} != {}", sm->startStateID, tgt->startStateId));
    if (sm->states._size < tgt->minStates)
        return skip(fmt::format("(a) states {} < {} (1인칭 등 다른 그래프)", sm->states._size, tgt->minStates));

    StateInfoLayout* startState = nullptr;
    std::unordered_set<std::int32_t> ids;
    for (int i = 0; i < sm->states._size; ++i) {
        auto* s = reinterpret_cast<StateInfoLayout*>(sm->states._data[i]);
        if (!s) continue;
        ids.insert(s->stateId);
        if (s->stateId == tgt->startStateId) startState = s;
    }
    if (!startState) return skip("(b) 시작 상태를 못 찾음");
    const char* startName = startState->name.c_str();
    if (!startName || _stricmp(startName, tgt->startStateName) != 0)
        return skip(
            fmt::format("(b) 시작 상태 이름 '{}' != '{}'", startName ? startName : "null", tgt->startStateName));
    if (!startState->generator) return skip("(b) 시작 상태 generator null");

    auto* wild = reinterpret_cast<TransitionInfoArrayLayout*>(sm->wildcardTransitions.get());
    if (tgt->firstWildcardEvent) {
        if (!wild || wild->transitions._size < 1) return skip("(c) 와일드카드 배열 없음");
        const auto fl0 = static_cast<std::uint16_t>(wild->transitions._data[0].flags);
        if ((fl0 & TransitionInfo::kIsLocalWildcard) == 0 || (fl0 & TransitionInfo::kDisableCondition) == 0)
            return skip(
                fmt::format("(c) 와일드카드[0] flags=0x{:X} 에 LOCAL_WILDCARD|DISABLE_CONDITION 비트 없음", fl0));
        const char* ev0 = EventNameOf(a_graph, wild->transitions._data[0].eventId);
        if (_stricmp(ev0, tgt->firstWildcardEvent) != 0)
            return skip(fmt::format("(c) 와일드카드[0] 이벤트 '{}' != '{}'", ev0, tgt->firstWildcardEvent));
    }
    logger::debug("[Inject] {}: 자가검증 통과 — states={} wildcard={}", tgt->smName, sm->states._size,
        wild ? wild->transitions._size : -1);
    if (!FrameworkSettings::Get().injectState) return skip("bInjectState=false");

    // Only real native transition events may be intercepted for cancel-out.
    if (!g_currentFirstPerson) {
        // Read the graph first, then merge the names under the publish lock.
        std::vector<std::string> names;
        auto collect = [&](TransitionInfoArrayLayout* list) {
            if (!list) return;
            for (int i = 0; i < list->transitions._size; ++i) {
                const char* name = EventNameOf(a_graph, list->transitions._data[i].eventId);
                if (!name || !*name || _strnicmp(name, "AI_", 3) == 0 || _strnicmp(name, "pa_AI_", 6) == 0) continue;
                names.emplace_back(name);
            }
        };
        collect(wild);
        collect(reinterpret_cast<TransitionInfoArrayLayout*>(startState->transitions));
        std::unique_lock lock(g_publishLock);
        for (auto& name : names)
            if (std::find(nativeTransitionEvents.begin(), nativeTransitionEvents.end(), name) ==
                nativeTransitionEvents.end())
                nativeTransitionEvents.push_back(std::move(name));
    }

    // Events go into this graph right before its first action (a validated target that no action
    // enters gets none).
    const bool rootTarget = std::strcmp(tgt->smName, "RootBehaviorGraph") == 0;
    // The node id budget: the graph's own nodes now, then every node an action makes. An action that might not fit
    // is left out whole, and the graph keeps working (see "Node id budget" above).
    const char* viewLabel = g_currentFirstPerson ? "first-person" : "third-person";
    // Test mode: the graph passes the id limit before any action is counted, as a load order with many behavior mods.
    if (const auto stress = static_cast<std::size_t>(FrameworkSettings::Get().nodeIdStressNodes);
        stress && rootTarget && !g_currentFirstPerson)
        NodeIdStress::Inflate(a_graph, sm, stress);
    const std::size_t nativeNodes = rootTarget ? CountNodes(root) : 0;
    const std::size_t createdBefore = CreatedNodes();
    const auto made = [&] { return CreatedNodes() - createdBefore; };
    // With core/NodeIdRenumber running, a graph past the node id limit is renumbered and only its state machines need
    // ids of their own: an action fits while they do.
    const bool renumbering = rootTarget && NodeIdRenumber::Active();
    const std::size_t nativeMachines = renumbering ? NodeIdRenumber::Machines(AllNodes(root)) : 0;
    const bool machinesFit =
        renumbering && nativeMachines + NodeIdRenumber::kInjectedMachinesReserve + NodeIdRenumber::kCloneIds <
                           NodeIdRenumber::kMachineIdLimit;
    std::vector<const std::string*> overBudget;
    const auto fits = [&](int action) {
        if (nativeNodes + made() + NodeReserve(action) + kNodeIdsKeptFree <= kNodeIdLimit) return true;
        if (machinesFit) return true;
        overBudget.push_back(&Actions::Spec(action).id);
        return false;
    };
    int firstPersonPairs = 0;
    for (int a = 0; a < Actions::Count(); ++a) {
        const auto& spec = Actions::Spec(a);
        if (spec.clips.empty()) continue;  // a selecting action has nothing to play (XmlV2, 0.12.244)
        if (spec.CreatureAction()) continue;
        if (spec.paired.enabled) {
            // A creature-started pair (11-05 phase 2) gives the character its partner role only, none to a pair
            // between creatures.
            if (!PairedRig::SupportsRole(spec.rig, spec.paired.targetRig, PairedRig::Kind::kCharacter, 0) &&
                !PairedRig::SupportsRole(spec.rig, spec.paired.targetRig, PairedRig::Kind::kCharacter, 1))
                continue;
            // Pairs play in third person; the first-person root gets none (one line below, not one each).
            if (rootTarget && g_currentFirstPerson)
                ++firstPersonPairs;
            else if (rootTarget && fits(a)) {
                RegisterEvents(a_graph);
                InjectPairedAction(a, a_graph, sm, wild, ids, a_character, tgt->startStateId);
            }
            continue;
        }
        // Each view's graphs get the actions that play in it (runtime/ViewSpec.h).
        if (g_currentFirstPerson ? !InFirst(spec.view.view) : !InThird(spec.view.view)) continue;
        if (!rootTarget || !fits(a)) continue;
        RegisterEvents(a_graph);
        if (!g_currentFirstPerson && spec.condition.inAir != 0)
            InjectAction(a, *tgt, a_graph, sm, startState, wild, ids, a_character, true);
        InjectFullBodyAction(a, a_graph, sm, wild, a_character);
    }
    if (firstPersonPairs)
        logger::info("[PairB] first-person {}: {} paired actions not injected (pairs play in third person)",
            tgt->smName, firstPersonPairs);
    if (!rootTarget) return;
    std::vector<std::string> leftOut;
    for (const auto* id : overBudget) leftOut.push_back(*id);
    // Counted again once the pass has committed its staged exits and pair containers: this is the count the engine
    // numbers (native + made misses native nodes an action newly reaches; a count before the commit misses the
    // committed parts). A test game 11-07: 7010 + 506 = 7516, counted before the commit 7244, engine 7549.
    pass.pass.afterCommit = [=, label = std::string(viewLabel), made = CreatedNodes() - createdBefore] {
        ReportNodeIds(a_graph, root, label.c_str(), nativeNodes, made, renumbering, !leftOut.empty());
        if (!leftOut.empty()) {
            std::string names;
            for (std::size_t i = 0; i < leftOut.size() && i < 40; ++i) names += (i ? ", " : "") + leftOut[i];
            if (leftOut.size() > 40) names += std::format(", ... ({} more)", leftOut.size() - 40);
            logger::error(
                "[NodeBudget] {} 0_master: {} actions left out — the behavior graph is full (Havok numbers its nodes "
                "with 16-bit ids; more would make two nodes share one and crash). Fewer behavior mods (Nemesis/"
                "Pandora) free room: {}",
                label, leftOut.size(), names);
        }
    };
}

// ── 훅 1: CreateSymbolIdMap ─────────────────────────────────────────────────
struct HkbSymbolIdMap;
struct HkbSymbolLinker;
using SymbolHookFn = HkbSymbolIdMap* (*)(RE::hkbBehaviorGraph*, const RE::BSScrapArray<RE::hkbBehaviorGraph*>&,
    const char*, RE::hkbCharacter*, HkbSymbolLinker*, HkbSymbolLinker*);
SymbolHookFn _origSymbol = nullptr;

using ProfileClock = std::chrono::steady_clock;
std::atomic<std::uint64_t> profileSerial{0};
double ElapsedMs(ProfileClock::time_point start) {
    return std::chrono::duration<double, std::milli>(ProfileClock::now() - start).count();
}
HkbSymbolIdMap* SymbolHook(RE::hkbBehaviorGraph* a_master, const RE::BSScrapArray<RE::hkbBehaviorGraph*>& a_graphs,
    const char* a_path, RE::hkbCharacter* a_character, HkbSymbolLinker* a_ev, HkbSymbolLinker* a_var) {
    const auto serial = ++profileSerial;
    const auto thread = GetCurrentThreadId();
    const bool mapsEmpty =
        a_master && a_master->data.get() && !a_master->eventIDMap.get() && !a_master->variableIDMap.get();
    const bool trace = mapsEmpty || FrameworkSettings::Get().debug;
    if (trace)
        logger::info("[InjectProfile] #{} Symbol begin thread={} path='{}' graphs={} actions={} mapsEmpty={}", serial,
            thread, a_path ? a_path : "(none)", a_graphs.size(), Actions::Count(), mapsEmpty);
    const auto filesStart = ProfileClock::now();
    int filesAdded = 0;
    if (Actions::Count() > 0) {
        auto* setup = a_character ? a_character->setup.get() : nullptr;
        auto* data = setup ? setup->data.get() : nullptr;
        filesAdded = RegisterAnimationFiles(data ? data->stringData.get() : nullptr, "pre-symbol");
    }
    const double filesMs = ElapsedMs(filesStart);
    const auto eventsStart = ProfileClock::now();
    // Projects that can receive an action (the character in both views, creatures that are pair
    // targets): the master graph gets the events now, injected graphs in the states pass.
    std::unordered_set<const void*> eventGraphs;
    EventCounts eventCounts;
    const auto* eventSetup = a_character ? a_character->setup.get() : nullptr;
    const auto* eventData = eventSetup ? eventSetup->data.get() : nullptr;
    const auto* eventStrings = eventData ? eventData->stringData.get() : nullptr;
    const auto eventRig = eventStrings && eventStrings->name.c_str()
                              ? PairedRig::CharacterName(eventStrings->name.c_str())
                              : PairedRig::Kind::kUnsupported;
    const bool firstPersonPath = a_path && (std::strstr(a_path, "_1stPerson") || std::strstr(a_path, "_1stperson") ||
                                               std::strstr(a_path, "_1STPERSON"));
    const bool actionProject = eventRig == PairedRig::Kind::kCharacter || firstPersonPath ||
                               (PairedRig::IsCreature(eventRig) && !Actions::AllClipFiles(eventRig).empty());
    struct EventScope {
        std::unordered_set<const void*>* graphs;
        EventCounts* counts;
        EventScope(std::unordered_set<const void*>* g, EventCounts* c) : graphs(g_eventGraphs), counts(g_eventCounts) {
            g_eventGraphs = g;
            g_eventCounts = c;
        }
        ~EventScope() {
            g_eventGraphs = graphs;
            g_eventCounts = counts;
        }
    } eventScope{mapsEmpty && actionProject ? &eventGraphs : nullptr, &eventCounts};
    if (mapsEmpty && actionProject && Actions::Count() > 0) RegisterEvents(a_master);
    const double eventsMs = ElapsedMs(eventsStart);
    const auto statesStart = ProfileClock::now();
    if (mapsEmpty && Actions::Count() > 0) {
        const bool previousFirstPerson = g_currentFirstPerson;
        g_currentFirstPerson = a_path && (std::strstr(a_path, "_1stPerson") || std::strstr(a_path, "_1stperson") ||
                                             std::strstr(a_path, "_1STPERSON"));
        // The character's animation names are looked up per injected clip: index them for the pass.
        auto* setup = a_character ? a_character->setup.get() : nullptr;
        auto* characterData = setup ? setup->data.get() : nullptr;
        auto* characterStrings = characterData ? characterData->stringData.get() : nullptr;
        std::optional<NameLookupScope> animationLookup;
        if (characterStrings) animationLookup.emplace(characterStrings->animationNames);
        for (auto* graph : a_graphs) {
            auto* data = graph ? graph->data.get() : nullptr;
            auto* strings = data ? data->stringData.get() : nullptr;
            if (strings) {
                NameLookupScope lookup(strings->eventNames);
                TryInjectGraph(graph, a_character);
            } else
                TryInjectGraph(graph, a_character);
        }
        g_currentFirstPerson = previousFirstPerson;
    } else if (mapsEmpty)
        MeasureCharacterProject(a_graphs, a_character, firstPersonPath);
    const double statesMs = ElapsedMs(statesStart);
    // The event registry's names go into every graph of the project: a registered name is an annotation
    // any clip may carry (a vanilla or OAR clip in a graph no action is injected into), and an
    // annotation is sent only when its graph has the name. Graphs that got the full list above have it.
    const auto actionEventGraphs = eventGraphs.size();  // the graphs that got every declared event
    const auto registryStart = ProfileClock::now();
    EventCounts registryCounts;
    std::size_t registryGraphs = 0;
    if (mapsEmpty && actionProject) {
        const auto& registry = Actions::RegistryEventNames();
        const auto addTo = [&](RE::hkbBehaviorGraph* graph) {
            if (!graph || eventGraphs.contains(graph)) return;
            auto* data = graph->data.get();
            auto* strings = data ? data->stringData.get() : nullptr;
            if (!strings) return;
            eventGraphs.insert(graph);
            ++registryGraphs;
            NameLookupScope lookup(strings->eventNames);
            for (const auto& name : registry) {
                const int before = strings->eventNames._size;
                const int id = InjectEvent(graph, name.c_str());
                if (id < 0)
                    ++registryCounts.failed;
                else if (strings->eventNames._size > before)
                    ++registryCounts.added;
                else
                    ++registryCounts.existing;
            }
        };
        if (!registry.empty()) {
            addTo(a_master);
            for (auto* graph : a_graphs) addTo(graph);
        }
        if (registryCounts.failed)
            logger::warn("[EventRegistry] '{}': {} registered names could not be added to {} graphs",
                a_path ? a_path : "(none)", registryCounts.failed, registryGraphs);
    }
    const double registryMs = ElapsedMs(registryStart);
    if (trace)
        logger::info(
            "[InjectProfile] #{} Symbol own filesMs={:.3f} eventsMs={:.3f} statesMs={:.3f} registryMs={:.3f} filesAdded={} eventGraphs={} eventsAdded={} existing={} failed={} registryGraphs={} registryAdded={} forward=begin",
            serial, filesMs, eventsMs, statesMs, registryMs, filesAdded, actionEventGraphs, eventCounts.added,
            eventCounts.existing, eventCounts.failed, registryGraphs, registryCounts.added);
    const auto forwardStart = ProfileClock::now();
    auto* result = _origSymbol(a_master, a_graphs, a_path, a_character, a_ev, a_var);
    const double forwardMs = ElapsedMs(forwardStart);
    if (trace || forwardMs >= 50.0 || filesMs + eventsMs + statesMs + registryMs >= 10.0)
        logger::info("[InjectProfile] #{} Symbol end thread={} path='{}' ownMs={:.3f} downstreamMs={:.3f}", serial,
            thread, a_path ? a_path : "(none)", filesMs + eventsMs + statesMs + registryMs, forwardMs);
    return result;
}

// ── 훅 2: LoadClips ─────────────────────────────────────────────────────────
using LoadClipsFn = void (*)(
    RE::hkbCharacterStringData*, RE::hkbAnimationBindingSet*, void*, RE::hkbBehaviorGraph*, const char*, void*);
LoadClipsFn _origLoadClips = nullptr;

void LoadClipsHook(RE::hkbCharacterStringData* a_stringData, RE::hkbAnimationBindingSet* a_bindingSet,
    void* a_assetLoader, RE::hkbBehaviorGraph* a_rootBehavior, const char* a_animationPath,
    void* a_annotationToEventIdMap) {
    const auto serial = ++profileSerial;
    const auto thread = GetCurrentThreadId();
    const char* project = a_stringData && a_stringData->name.c_str() ? a_stringData->name.c_str() : "(none)";
    // Every project load passes here: one INFO line when it registered our files or took long, as the
    // symbol hook does; the begin and step lines only with bDebug (code review 2026-09-30 R-21).
    const bool trace = FrameworkSettings::Get().debug;
    if (trace)
        logger::info("[InjectProfile] #{} LoadClips begin thread={} project='{}' path='{}' names={}", serial, thread,
            project, a_animationPath ? a_animationPath : "(none)",
            a_stringData ? a_stringData->animationNames._size : -1);
    const auto filesStart = ProfileClock::now();
    const int added = RegisterAnimationFiles(a_stringData, "pre-load");
    const double filesMs = ElapsedMs(filesStart);
    if (trace)
        logger::info(
            "[InjectProfile] #{} LoadClips filesMs={:.3f} filesAdded={} forward=begin", serial, filesMs, added);
    const auto forwardStart = ProfileClock::now();
    _origLoadClips(
        a_stringData, a_bindingSet, a_assetLoader, a_rootBehavior, a_animationPath, a_annotationToEventIdMap);
    const double forwardMs = ElapsedMs(forwardStart);
    const auto postStart = ProfileClock::now();
    if (a_animationPath && _strnicmp(a_animationPath, "Actors", 6) == 0) AnimData::TryRegister("LoadClips");
    if (trace || added || forwardMs >= 50.0 || filesMs >= 10.0)
        logger::info(
            "[InjectProfile] #{} LoadClips end thread={} project='{}' filesMs={:.3f} filesAdded={} downstreamMs={:.3f} postMs={:.3f} bindings={}",
            serial, thread, project, filesMs, added, forwardMs, ElapsedMs(postStart),
            a_bindingSet ? a_bindingSet->bindings._size : -1);
}

}

void Install() {
    auto& tr = SKSE::GetTrampoline();
    REL::Relocation<std::uintptr_t> sym{REL::RelocationID(62640, 63585)};
    const std::uintptr_t symbolSite = sym.address() + REL::Relocate(0x22A, 0x237);
    // Without this call site no state can be injected; refuse rather than overwrite other code.
    if (*reinterpret_cast<const std::uint8_t*>(symbolSite) != 0xE8) {
        logger::error("[Inject] CreateSymbolIdMap 호출부가 call 이 아님 — 삽입 훅 미설치");
        return;
    }
    _origSymbol = reinterpret_cast<SymbolHookFn>(tr.write_call<5>(symbolSite, SymbolHook));

    REL::Relocation<std::uintptr_t> lc{REL::RelocationID(63028, 63846)};
    const std::uintptr_t site = lc.address() + REL::Relocate(0x8D, 0x101);
    if (*reinterpret_cast<const std::uint8_t*>(site) != 0xE8) {
        logger::error("[Inject] LoadClips 호출부가 call 이 아님 — 훅 미설치");
        return;
    }
    _origLoadClips = reinterpret_cast<LoadClipsFn>(tr.write_call<5>(site, LoadClipsHook));
    logger::debug("[Inject] hooks installed (CreateSymbolIdMap, LoadClips)");
}

std::vector<SyncNode> SyncNodes() {
    std::shared_lock lock(g_publishLock);
    return g_syncNodes;
}
bool IsActionState(int action, const char* graph, std::int32_t stateId) {
    if (!graph) return false;
    std::shared_lock lock(g_publishLock);
    return AnyResultOf(action, [&](const Result& r) {
        return r.injected && r.graph == graph &&
               std::find(r.stateIds.begin(), r.stateIds.end(), stateId) != r.stateIds.end();
    });
}
bool IsNativeTransitionEvent(const char* event) {
    if (!event) return false;
    std::shared_lock lock(g_publishLock);
    for (const auto& name : nativeTransitionEvents)
        if (_stricmp(name.c_str(), event) == 0) return true;
    return false;
}
bool HasFullBodyRoute(int action) {
    std::shared_lock lock(g_publishLock);
    return AnyResultOf(action, [](const Result& r) { return r.injected && r.graph == "FullBodyPlayerIdles_Behavior"; });
}
bool AnyInjected(int a_actionIdx) {
    std::shared_lock lock(g_publishLock);
    return AnyResultOf(a_actionIdx, [](const Result& r) { return r.injected; });
}
bool HasPairedRoutes(int action) {
    if (action < 0 || action >= Actions::Count()) return false;
    // Both roles' graphs: the initiator's (the character, or a creature since 11-05 phase 2) and the partner's.
    const auto& spec = Actions::Spec(action);
    bool initiator = false, target = false;
    std::shared_lock lock(g_publishLock);
    AnyResultOf(action, [&](const Result& r) {
        if (r.injected) {
            initiator |= r.pairedRig == spec.rig;
            target |= r.pairedRig == spec.paired.targetRig;
        }
        return false;
    });
    return initiator && target;
}
bool HasAirRoute(int a_actionIdx) {
    std::shared_lock lock(g_publishLock);
    return AnyResultOf(a_actionIdx, [](const Result& r) { return r.injected && r.airRoute; });
}
bool IsOurStateId(const char* a_smName, std::int32_t a_stateId) {
    std::shared_lock lock(g_publishLock);
    for (auto& r : g_results)
        if (r.injected && _stricmp(r.graph.c_str(), a_smName) == 0)
            for (auto s : r.stateIds)
                if (s == a_stateId) return true;
    return false;
}
std::vector<RE::hkbNode*> AllNodes(RE::hkbNode* root) {
    std::vector<RE::hkbNode*> nodes;
    std::unordered_set<RE::hkbNode*> seen;
    std::vector<RE::hkbNode*> stack{root};
    std::vector<HkChild> buffer;
    while (!stack.empty()) {
        auto* node = stack.back();
        stack.pop_back();
        if (!node || !seen.insert(node).second) continue;
        nodes.push_back(node);
        const int count = ListChildren(node, buffer, 1 << 20);
        for (int i = count - 1; i >= 0; --i) stack.push_back(buffer[i].node);
    }
    return nodes;
}

}  // namespace AnimationInjector::GraphInjector
