#include "core/PairRootRuntime.h"
#include "core/NativePairAlignment.h"
#include "core/NativePairEnd.h"
#include "core/PairRootScale.h"
#include "PCH.h"
#include "core/ExitBlendHooks.h"
#include "runtime/InputSink.h"
#include "runtime/ActionRegistry.h"
#include "runtime/ActionRunner.h"
#include "runtime/ClipIndex.h"
#include "core/EventMaps.h"
#include "core/AirControl.h"
#include "core/SpaceProbe.h"
#include "Settings.h"
#include <mutex>
#include "runtime/FireArrow.h"
#include "runtime/PairedSession.h"
#include "runtime/Carry.h"
#include "runtime/PairFeatures.h"
#include "runtime/ActionLinks.h"
#include "runtime/Qte.h"
#include "runtime/GetUpHandoff.h"
#include "runtime/FlowLinkRoutes.h"
#include "runtime/DeclarationCatalog.h"
#include "runtime/EventRegistry.h"
#include "runtime/EquipmentCondition.h"
#include "runtime/EventSuppression.h"
#include "runtime/ExtendedConditions.h"
#include "runtime/BoundedMailbox.h"
#include "runtime/MainThread.h"
#include "runtime/RunnerSet.h"
#include "runtime/ViewGuard.h"
#include <atomic>
#include <future>
#include <shared_mutex>

std::uint32_t AnimationInjector::MainThread::CurrentId() { return ::GetCurrentThreadId(); }

namespace AnimationInjector::Actions {
bool HasChance(int action) {
    if (action < 0 || action >= Count()) return false;
    const auto& predicates = Spec(action).flow.predicates;
    return std::any_of(
        predicates.begin(), predicates.end(), [](const auto& p) { return p.test == ConditionFlowXml::Test::kChance; });
}
RE::ActorHandle CaptureTarget(RE::Actor* actor, int action, RE::ActorHandle partner) {
    if (partner.native_handle() != 0 || action < 0 || action >= Count() || !actor || !actor->Is3DLoaded())
        return partner;
    const auto& spec = Spec(action);
    if (spec.flow.nodes.empty())
        return spec.paired.enabled ? ActionRunner::PairedTargetFor(actor, action) : RE::ActorHandle{};
    // Search actual Play destinations before race/health evaluation. Retain
    // the nearest candidate across rigs; failed branches never retarget.
    RE::ActorHandle best;
    float distance = std::numeric_limits<float>::max();
    bool paired = false;
    std::vector<int> visited;
    for (const auto& node : spec.flow.nodes) {
        if (node.kind != ConditionFlow::Kind::kAction || node.action < 0 ||
            node.action >= static_cast<int>(spec.flowActions.size()))
            continue;
        const int i = IndexOf(spec.flowActions[node.action]);
        if (i < 0 || std::find(visited.begin(), visited.end(), i) != visited.end()) continue;
        visited.push_back(i);
        if (!Spec(i).paired.enabled) continue;
        paired = true;
        auto handle = ActionRunner::PairedTargetFor(actor, i);
        auto candidate = handle.get();
        if (!candidate) continue;
        const float d = actor->GetPosition().GetDistance(candidate->GetPosition());
        if (d < distance) {
            best = handle;
            distance = d;
        }
    }
    if (paired) return best;
    return actor->GetActorRuntimeData().currentCombatTarget;
}

int ResolveFlow(
    RE::Actor* actor, int action, RE::ActorHandle partner, bool requireAvailable, ConditionValues::Request* request) {
    if (action < 0 || action >= Count() || !actor || !actor->Is3DLoaded()) return -1;
    const auto& spec = Spec(action);
    if (spec.flow.nodes.empty()) return action;
    auto other = partner.get();
    ConditionValues::Request localRequest;
    auto& random = request ? *request : localRequest;
    if (!random.movementCaptured) {
        auto* player = RE::PlayerCharacter::GetSingleton();
        random.CaptureMovement(
            player && player->Is3DLoaded() ? static_cast<int>(InputSink::Get()->CurrentMoveDir(MoveDir::kNone)) : -1);
    }
    const auto result = ConditionFlow::Select(
        spec.flow.nodes, spec.flow.entry, spec.flow.predicates.size(), spec.flowActions.size(), [&](int index) {
            using Value = ConditionFlow::Value;
            using Test = ConditionFlowXml::Test;
            const auto& p = spec.flow.predicates[index];
            auto* subject = p.subject == ConditionFlowXml::Subject::kSelf ? actor : other.get();
            if (!subject || !subject->Is3DLoaded()) return Value::kUnavailable;
            if (p.test >= Test::kRig)
                return ExtendedConditions::Evaluate(p, subject,
                    p.subject == ConditionFlowXml::Subject::kSelf ? other.get() : actor, action, index, random);
            bool answer = false;
            switch (p.test) {
            case Test::kEquippedType: {
                if (!subject->GetActorRuntimeData().currentProcess) return Value::kUnavailable;
                auto* equipped = subject->GetEquippedObject(p.leftHand);
                answer = MatchesEquippedType(equipped, p.equippedType);
                logger::debug("[FlowTest] actor={:08X} test=equippedType left={} form={:08X} type={} result={}",
                    subject->GetFormID(), p.leftHand, equipped ? equipped->GetFormID() : 0u,
                    static_cast<int>(p.equippedType), answer);
                break;
            }
            case Test::kAttackState: {
                auto* state = subject->AsActorState();
                if (!state) return Value::kUnavailable;
                answer = state->GetAttackState() == NativeAttackState(p.attackState);
                logger::debug("[FlowTest] actor={:08X} test=attackState actual={} expected={} result={}",
                    subject->GetFormID(), static_cast<int>(state->GetAttackState()),
                    static_cast<int>(NativeAttackState(p.attackState)), answer);
                break;
            }
            case Test::kSneaking: answer = subject->IsSneaking(); break;
            case Test::kFemale: {
                auto* base = subject->GetActorBase();
                if (!base) return Value::kUnavailable;
                const auto sex = base->GetSex();
                if (sex != RE::SEX::kFemale && sex != RE::SEX::kMale) return Value::kUnavailable;
                answer = sex == RE::SEX::kFemale;
                break;
            }
            case Test::kUnarmedRight: {
                auto* equipped = subject->GetEquippedObject(false);
                auto* weapon = equipped ? equipped->As<RE::TESObjectWEAP>() : nullptr;
                answer = !equipped || (weapon && weapon->GetWeaponType() == RE::WEAPON_TYPE::kHandToHandMelee);
                break;
            }
            case Test::kMagicEffect:
            case Test::kGlobal: {
                auto* data = RE::TESDataHandler::GetSingleton();
                if (!data) return Value::kUnavailable;
                auto* form = data->LookupForm(p.form, p.plugin);
                if (!form) {
                    logger::debug(
                        "[FormCondition] actor={:08X} flow={} plugin={} form={:X} result=unavailable reason=missing-form",
                        subject->GetFormID(), spec.id, p.plugin, p.form);
                    return Value::kUnavailable;
                }
                if (p.test == Test::kMagicEffect) {
                    auto* effect = form->As<RE::EffectSetting>();
                    auto* magic = subject->AsMagicTarget();
                    if (!effect || !magic) {
                        logger::debug(
                            "[FormCondition] actor={:08X} flow={} plugin={} form={:X} result=unavailable reason=effect-type",
                            subject->GetFormID(), spec.id, p.plugin, p.form);
                        return Value::kUnavailable;
                    }
                    answer = magic->HasMagicEffect(effect);
                } else {
                    auto* global = form->As<RE::TESGlobal>();
                    if (!global || !std::isfinite(global->value)) {
                        logger::debug(
                            "[FormCondition] actor={:08X} flow={} plugin={} form={:X} result=unavailable reason=global-type-or-value",
                            subject->GetFormID(), spec.id, p.plugin, p.form);
                        return Value::kUnavailable;
                    }
                    answer = global->value == p.equals;
                }
                logger::debug("[FormCondition] actor={:08X} flow={} plugin={} form={:X} kind={} result={}",
                    subject->GetFormID(), spec.id, p.plugin, p.form,
                    p.test == Test::kMagicEffect ? "magicEffect" : "global", answer);
                break;
            }
            case Test::kSpace:
            case Test::kGround: {
                if (p.candidate < 0 || p.candidate >= static_cast<int>(spec.flowActions.size()))
                    return Value::kUnavailable;
                const int candidate = IndexOf(spec.flowActions[p.candidate]);
                const int role = p.subject == ConditionFlowXml::Subject::kSelf ? 0 : 1;
                const auto value = p.test == Test::kGround ? SpaceProbe::EvaluateGround(actor->GetHandle(), partner,
                                                                 candidate, role, p.distance, p.direction, p.ground)
                                                           : SpaceProbe::Evaluate(actor->GetHandle(), partner,
                                                                 candidate, role, p.distance, p.direction);
                logger::debug("[SpaceCondition] actor={:08X} flow={} candidate={} subject={} distance={} verdict={}",
                    actor->GetFormID(), spec.id, spec.flowActions[p.candidate],
                    p.subject == ConditionFlowXml::Subject::kSelf ? "self" : "partner", p.distance,
                    value == Value::kTrue    ? "clear"
                    : value == Value::kFalse ? "blocked"
                                             : "unavailable");
                return value;
            }
            case Test::kAlive: answer = !subject->IsDead(); break;
            case Test::kCombat: answer = subject->IsInCombat(); break;
            case Test::kWeaponDrawn: {
                auto* state = subject->AsActorState();
                if (!state) return Value::kUnavailable;
                answer = state->IsWeaponDrawn();
                break;
            }
            case Test::kAir: {
                auto* controller = subject->GetCharController();
                if (!controller) return Value::kUnavailable;
                answer = controller->context.currentState == RE::hkpCharacterStateType::kInAir;
                break;
            }
            case Test::kStamina:
            case Test::kMagicka: {
                auto* values = subject->AsActorValueOwner();
                if (!values) return Value::kUnavailable;
                const auto value = values->GetActorValue(
                    p.test == Test::kStamina ? RE::ActorValue::kStamina : RE::ActorValue::kMagicka);
                if (!std::isfinite(value)) return Value::kUnavailable;
                answer = value >= p.minimum;
                break;
            }
            }
            return answer ? Value::kTrue : Value::kFalse;
        });
    if (result.action < 0 || result.error != ConditionFlow::Error::kNone || (requireAvailable && result.unavailable))
        return -1;
    const auto& name = spec.flowActions[result.action];
    const int i = IndexOf(name);
    if (i < 0 || (i != action && !Spec(i).flow.nodes.empty())) return -1;
    logger::debug(
        "[Flow] actor={:08X} from={} to={} unavailable={}", actor->GetFormID(), spec.id, name, result.unavailable);
    return i;
}

int SelectStart(RE::Actor* actor, int source, RE::ActorHandle partner, ConditionValues::Request* request) {
    auto* origin = RunnerFor(actor, source, true);
    if (!origin || !origin->CanRequest()) return -1;
    const auto target = CaptureTarget(actor, source, partner);
    const int selected = ResolveFlow(actor, source, target, !origin->Spec().suppress.events.empty(), request);
    auto* destination = RunnerFor(actor, selected, true);
    if (!destination) return -1;
    if (destination->Spec().paired.enabled)
        return destination->CanRequest() && destination->CanRequestPairLink(target) ? selected : -1;
    return destination->CanStart() ? selected : -1;
}

int SelectPairedStart(RE::Actor* actor, int source, RE::ActorHandle partner, ConditionValues::Request* request) {
    if (source < 0 || source >= Count()) return -1;
    if (!Spec(source).flow.nodes.empty()) {
        auto* origin = RunnerFor(actor, source, true);
        if (!origin || !origin->CanRequest()) return -1;
    }
    return ResolveFlow(actor, source, partner, false, request);
}

namespace {
std::vector<ActionSpec> g_specs;
// Built with g_specs and read-only afterwards (hooks on any thread).
ClipIndex g_clipIndex;
DeclarationCatalog g_catalog;
std::unordered_map<std::string, int> g_idIndex;  // exact id -> action
std::unordered_map<std::uint32_t, std::vector<int>> g_keyActions, g_padActions;
std::vector<int> g_fireArrowActions;
std::vector<std::vector<std::pair<int, int>>> g_incomingLinks;  // action -> (source, link) naming it
const std::vector<int> kNoActions;

// Declarations load off the main thread (0.12.208): SKSEPluginLoad starts the load and the first
// reader of any of the tables above waits for it, so a partly loaded set is never seen. Reading
// thousands of declarations no longer holds up game start; the first graph load, which is the
// first reader, comes after the game's own data load.
struct Declared {
    std::vector<ActionSpec> specs;
    std::vector<EventRuleSpec> events;  // every file's EventRegistry, merged by EventRegistry::Build
    ClipIndex clips;
    DeclarationCatalog catalog;
    std::unordered_map<std::string, int> ids;
    std::unordered_map<std::uint32_t, std::vector<int>> keyActions, padActions;
    std::vector<int> fireArrowActions;
    std::vector<std::vector<std::pair<int, int>>> incomingLinks;
    double loadMs = 0, catalogMs = 0;
};
Declared BuildDeclarations() {
    using Clock = std::chrono::steady_clock;
    Declared d;
    const auto loadStart = Clock::now();
    d.specs = Declarations::LoadAll(&d.events);
    ExpandFlowLinkRoutes(d.specs);
    d.clips.Build(d.specs);
    // Lookups that used to walk every action: by id, by start key or button, fire-arrow actions and
    // the links that name each action.
    for (int i = 0; i < static_cast<int>(d.specs.size()); ++i) {
        const auto& spec = d.specs[i];
        d.ids.try_emplace(spec.id, i);
        d.keyActions[spec.trigger.key].push_back(i);
        if (spec.trigger.gamepad) d.padActions[spec.trigger.gamepad].push_back(i);
        if (spec.fireArrow.enabled) d.fireArrowActions.push_back(i);
    }
    d.incomingLinks.assign(d.specs.size(), {});
    for (int from = 0; from < static_cast<int>(d.specs.size()); ++from)
        for (int j = 0; j < static_cast<int>(d.specs[from].links.size()); ++j)
            if (const auto to = d.ids.find(d.specs[from].links[j].action); to != d.ids.end())
                d.incomingLinks[to->second].push_back({from, j});
    d.loadMs = std::chrono::duration<double, std::milli>(Clock::now() - loadStart).count();
    const auto catalogStart = Clock::now();
    std::vector<std::string> registered;
    registered.reserve(d.events.size());
    for (const auto& rule : d.events)
        for (auto& name : GraphNames(rule)) registered.push_back(std::move(name));
    d.catalog.Build(d.specs, registered);
    d.catalogMs = std::chrono::duration<double, std::milli>(Clock::now() - catalogStart).count();
    return d;
}
std::future<Declared> g_loading;
std::atomic_bool g_loaded{false};
std::once_flag g_publishOnce;
void Publish() {
    const auto waitStart = std::chrono::steady_clock::now();
    auto d = g_loading.get();
    const double waited =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - waitStart).count();
    g_specs = std::move(d.specs);
    g_clipIndex = std::move(d.clips);
    g_catalog = std::move(d.catalog);
    g_idIndex = std::move(d.ids);
    g_keyActions = std::move(d.keyActions);
    g_padActions = std::move(d.padActions);
    g_fireArrowActions = std::move(d.fireArrowActions);
    g_incomingLinks = std::move(d.incomingLinks);
    EventSuppression::Load(g_specs);
    EventRegistry::Build(d.events);
    logger::info("[InjectProfile] declaration catalog actions={} files={} events={} distinct={} buildMs={:.3f}",
        g_specs.size(), g_catalog.files.size(), g_catalog.events.size(), g_catalog.uniqueEvents.size(), d.catalogMs);
    logger::info("[Actions] 액션 {}개 로드 (background loadMs={:.1f}, first reader waited {:.1f} ms on thread {})",
        g_specs.size(), d.loadMs, waited, MainThread::CurrentId());
    g_loaded.store(true, std::memory_order_release);
}
// Every reader of the declaration tables comes through here first.
void EnsureLoaded() {
    if (!g_loaded.load(std::memory_order_acquire)) std::call_once(g_publishOnce, Publish);
}
using Runners = RunnerSet<ActionRunner>;
auto MakeRunner(RE::ActorHandle owner, bool player) {
    return [owner, player](int action) { return std::make_unique<ActionRunner>(action, owner, player); };
}
// The player's runners: a runner exists once its action is used and is never removed during play,
// so readers on any thread may use the set after the bound flag is published.
std::shared_ptr<Runners> g_player;
RE::ActorHandle g_playerHandle;
std::atomic_bool g_playerBound{false};
std::mutex g_bindMutex;
using TriggerRequest = CapturedTrigger;
std::mutex triggerMutex;
std::vector<TriggerRequest> triggerQueue;
struct NativeReplay {
    RE::ActorHandle actor;
    RE::BSFixedString event;
};
std::vector<NativeReplay> nativeQueue;
void DrainNativeReplays() {
    std::vector<NativeReplay> queued;
    {
        std::lock_guard lock(triggerMutex);
        queued.swap(nativeQueue);
    }
    for (const auto& item : queued)
        if (auto actor = item.actor.get();
            actor && actor->Is3DLoaded() && !actor->IsDead() && !actor->IsInRagdollState()) {
            PairedSession::Ticket ticket;
            const bool busy = PairedSession::TryGetTicket(item.actor, ticket) ||
                              FindRunner(actor.get(),
                                  [](int, ActionRunner& r) { return r.GetState() != ActionRunner::State::kIdle; });
            if (!busy)
                logger::debug("[NativeCancel] replay actor={:08X} event='{}' accepted={}", actor->GetFormID(),
                    item.event.c_str(), actor->NotifyAnimationGraph(item.event));
        }
}
void DrainTriggers() {
    std::vector<TriggerRequest> queued;
    {
        std::lock_guard lock(triggerMutex);
        queued.swap(triggerQueue);
    }
    for (auto item : queued)
        if (auto actor = item.actor.get(); actor && actor->Is3DLoaded() && !actor->IsDead()) {
            const int origin = item.action;
            item.target = CaptureTarget(actor.get(), item.action, item.target);
            ConditionValues::Request request;
            request.CaptureSprinting(item.sprinting);
            item.action = g_specs[origin].paired.enabled ? SelectPairedStart(actor.get(), origin, item.target, &request)
                                                         : SelectStart(actor.get(), origin, item.target, &request);
            if (item.action < 0) continue;
            const auto& spec = g_specs[item.action];
            if (auto* runner = RunnerFor(actor.get(), item.action, true)) {
                if (spec.paired.enabled && !runner->ValidPairedTarget(item.target)) continue;
                const bool accepted =
                    spec.paired.enabled ? runner->RequestPaired(item.target) : runner->Request(origin);
                logger::debug("[EventTrigger] action='{}' event='{}' actor={:08X} paired={} accepted={}", spec.id,
                    spec.trigger.onEvent, actor->GetFormID(), spec.paired.enabled, accepted);
            }
        }
}

// NPC runners: an entry per actor (FormID), made on a trigger or an external entry, holding a runner
// for each action it uses; Update drops the entry once it is idle. The lock guards only the map and
// is never held across a runner call (runners call the engine, which can re-enter through clip
// hooks). Entries are shared so a reader on another thread keeps its runner alive while the main
// thread removes an idle entry.
struct NpcEntry {
    NpcEntry(RE::ActorHandle owner, int count) : handle(owner), runners(count) {}
    RE::ActorHandle handle;
    Runners runners;
    float idle = 0.0f;
};
std::unordered_map<RE::FormID, std::shared_ptr<NpcEntry>> g_npc;
std::shared_mutex g_npcLock;
std::shared_ptr<NpcEntry> FindNpc(RE::FormID id) {
    std::shared_lock lock(g_npcLock);
    const auto it = g_npc.find(id);
    return it != g_npc.end() ? it->second : nullptr;
}
std::vector<std::pair<RE::FormID, std::shared_ptr<NpcEntry>>> NpcSnapshot() {
    std::shared_lock lock(g_npcLock);
    return {g_npc.begin(), g_npc.end()};
}

// Clip and animation-event callbacks from worker threads, applied by the main thread.
struct RunnerCallback {
    enum class Kind : std::uint8_t { kActivated, kDeactivated, kAnimEvent };
    Kind kind;
    RE::ActorHandle actor;
    int action = -1, variant = -1;
    float duration = 0.0f;
    RE::BSFixedString event;
    bool moves = false;  // kActivated: the clip's HKX carries AMR motion
};
BoundedMailbox<RunnerCallback> g_callbacks{4096};

struct EventTrigger {
    int action;
    RE::BSFixedString tag;
    bool player, npc;
};
// onEvent triggers, built on first use (the tags are interned then). Every animation event of every
// actor looks itself up here: BSFixedString equality is identity of the interned string, so the
// interned text pointer is an exact key for a non-empty tag.
struct EventTriggerTable {
    std::vector<EventTrigger> triggers;
    std::unordered_map<const char*, std::vector<std::size_t>> byTag;  // in table (action) order
};
const EventTriggerTable& EventTriggers() {
    static const EventTriggerTable table = [] {
        EventTriggerTable t;
        for (int a = 0; a < static_cast<int>(g_specs.size()); ++a) {
            const auto& tr = g_specs[a].trigger;
            if (tr.onEvent.empty()) continue;
            t.triggers.push_back({a, RE::BSFixedString{tr.onEvent.c_str()}, tr.onEventPlayer, tr.onEventNpc});
            t.byTag[t.triggers.back().tag.c_str()].push_back(t.triggers.size() - 1);
        }
        return t;
    }();
    return table;
}

void BindPlayer() {
    if (g_playerBound.load(std::memory_order_acquire)) return;
    auto* p = RE::PlayerCharacter::GetSingleton();
    if (!p) return;
    std::lock_guard lock(g_bindMutex);
    if (g_playerBound.load(std::memory_order_relaxed)) return;
    g_playerHandle = p->GetHandle();
    g_player = std::make_shared<Runners>(Count());
    g_playerBound.store(true, std::memory_order_release);
}
ActionRunner* PlayerRunner(int action, bool create) {
    BindPlayer();
    if (!g_playerBound.load(std::memory_order_acquire)) return nullptr;
    return create ? g_player->Get(action, MakeRunner(g_playerHandle, true)) : g_player->Find(action);
}

// Main thread only. Same order as the former direct calls from the clip hooks.
void ApplyClipActivated(RE::Actor* actor, int action, int variant, float duration, bool moves) {
    auto* runner = Actions::RunnerFor(actor, action, true);  // NPC 는 여기서 실행기가 생긴다 (외부 진입 이벤트 경로)
    if (!runner) return;
    if (runner->GetState() == ActionRunner::State::kIdle)
        runner->Adopt(variant);  // 우리가 요청하지 않은 진입 → 떠맡는다
    runner->OnClipActivated(variant, duration, moves);
}
void ApplyClipDeactivated(RE::Actor* actor, int action, int variant) {
    if (auto* runner = Actions::RunnerFor(actor, action, false)) runner->OnClipDeactivated(variant);
}
void ApplyAnimEvent(RE::Actor* actor, const RE::BSFixedString& tag) {
    if (actor->IsPlayerRef()) {
        BindPlayer();
        if (g_playerBound.load(std::memory_order_acquire))
            g_player->ForEach([&](int, ActionRunner& r) { r.OnAnimEvent(tag); });
        return;
    }
    if (const auto entry = FindNpc(actor->GetFormID()))
        entry->runners.ForEach([&](int, ActionRunner& r) { r.OnAnimEvent(tag); });
}
// A full queue counts the drop; DrainCallbacks reports it.
void PostCallback(RunnerCallback callback) { g_callbacks.Post(std::move(callback)); }
void DrainCallbacks() {
    if (const auto dropped = g_callbacks.TakeDropped())
        logger::warn("[ThreadAffinity] {} runner callbacks dropped: queue full", dropped);
    for (auto& item : g_callbacks.TakeAll()) {
        auto actor = item.actor.get();
        if (!actor || !actor->Is3DLoaded()) continue;
        switch (item.kind) {
        case RunnerCallback::Kind::kActivated:
            ApplyClipActivated(actor.get(), item.action, item.variant, item.duration, item.moves);
            break;
        case RunnerCallback::Kind::kDeactivated: ApplyClipDeactivated(actor.get(), item.action, item.variant); break;
        case RunnerCallback::Kind::kAnimEvent: ApplyAnimEvent(actor.get(), item.event); break;
        }
    }
}
// First callback of each kind that arrives off the main thread is logged once per session,
// so the actual threading of clip and event hooks shows up in ordinary logs.
void NoteOffMain(RunnerCallback::Kind kind) {
    static std::array<std::atomic_bool, 3> seen{};
    if (!seen[static_cast<std::size_t>(kind)].exchange(true))
        logger::info("[ThreadAffinity] {} callback arrived off the main thread; applied on the next update",
            kind == RunnerCallback::Kind::kActivated     ? "clip-activate"
            : kind == RunnerCallback::Kind::kDeactivated ? "clip-deactivate"
                                                         : "anim-event");
}

// NPC runners are updated without the map lock: a runner may notify its graph, and the
// resulting synchronous clip hook looks the runner up again.
void UpdateNpcRunners(float delta) {
    std::vector<RE::FormID> stale;
    for (auto& [id, entry] : NpcSnapshot()) {
        auto actorPtr = entry->handle.get();
        RE::Actor* actor = actorPtr ? actorPtr.get() : nullptr;
        bool busy = false;
        if (actor && actor->Is3DLoaded()) {
            entry->runners.ForEach([&](int, ActionRunner& r) {
                r.Update(delta);
                busy = busy || r.Busy();
            });
        } else {
            entry->runners.ForEach([](int, ActionRunner& r) { r.ResetOnLoad(); });
        }
        entry->idle = busy ? 0.0f : entry->idle + delta;
        if (entry->idle > 2.0f) stale.push_back(id);
    }
    if (stale.empty()) return;
    std::unique_lock lock(g_npcLock);
    for (const auto id : stale) {
        const auto it = g_npc.find(id);
        if (it == g_npc.end() || it->second->idle <= 2.0f) continue;
        logger::debug("[Actions] NPC {:08X} 실행기 정리", id);
        g_npc.erase(it);
    }
}

// 애니 이벤트 전역 수신: Character/PlayerCharacter 의 BSTEventSink<BSAnimationGraphEvent>(vtable[2]) vfunc 1.
//   per-actor AddAnimationGraphEventSink 는 셀 전환 때 매니저가 재생성돼 싱크가 사라진다 → 전역 훅 (메모리 reference_anim_event_system)
template<class T>
struct AnimEventHook {
    static RE::BSEventNotifyControl thunk(RE::BSTEventSink<RE::BSAnimationGraphEvent>* a_sink,
        RE::BSAnimationGraphEvent* a_ev, RE::BSTEventSource<RE::BSAnimationGraphEvent>* a_src) {
        if (a_ev && a_ev->holder) {
            auto* actor = const_cast<RE::Actor*>(a_ev->holder->As<RE::Actor>());
            if (actor) {
                PairedSession::Ticket ticket;
                if (PairedSession::TryGetTicket(actor->GetHandle(), ticket))
                    PairedSession::ObserveEvent(ticket, actor->GetHandle(), a_ev->tag);
                OnAnimEventFrom(actor, a_ev->tag);
            }
        }
        const auto result = func(a_sink, a_ev, a_src);
        if (a_ev && a_ev->holder) {
            static const RE::BSFixedString arrowRelease("arrowRelease");
            if (a_ev->tag == arrowRelease)
                FireArrow::ObserveArrowRelease(const_cast<RE::Actor*>(a_ev->holder->As<RE::Actor>()));
        }
        return result;
    }
    static inline REL::Relocation<decltype(thunk)> func;
};

struct
    PlayerUpdateHook {  // PlayerCharacter::Update (vtable 0xAD) — 플레이어·NPC 실행기 전부 여기서 갱신 (델타는 같은 프레임)
    static void thunk(RE::PlayerCharacter* a_player, float a_delta) {
        if (const auto previous = MainThread::Confirm())
            logger::info(
                "[ThreadAffinity] runner updates run on thread {} (captured {})", MainThread::CurrentId(), previous);
        NativePairAlignment::BeforeUpdate();
        Carry::BeforeUpdate();
        auto* inputUI = RE::UI::GetSingleton();
        ExitBlendHooks::SetMoveInput(a_player && a_player->Is3DLoaded() && !a_player->IsDead() &&
                                     !a_player->IsInRagdollState() && inputUI && !inputUI->GameIsPaused() &&
                                     !inputUI->IsMenuOpen(RE::Console::MENU_NAME) && InputSink::Get()->AnyMoveInput());
        if (a_player && a_player->Is3DLoaded() && inputUI && !inputUI->GameIsPaused()) {
            ExitBlendHooks::Tick(a_delta);
            ExitBlendHooks::Drain();
            PairFeatures::MaintainMovementRecovery();
            Carry::Sample();
        }
        func(a_player, a_delta);
        auto* ui = RE::UI::GetSingleton();
        if (!a_player || !a_player->Is3DLoaded() || !ui || ui->GameIsPaused()) return;
        Carry::Sample();
        PairRootRuntime::Sample(a_delta, false);
        // Apply worker-thread clip results first so a finished runner can accept new triggers.
        DrainCallbacks();
        DrainTriggers();
        DrainNativeReplays();
        ExitBlendHooks::Drain();
        // Drain final annotations and cumulative samples before session cleanup discards them.
        PairFeatures::Update(a_delta);
        EventRegistry::Update(a_delta);
        PairedSession::Update(a_delta);
        PairRootScale::Update();
        if (g_playerBound.load(std::memory_order_acquire))
            g_player->ForEach([&](int, ActionRunner& r) { r.Update(a_delta); });
        UpdateNpcRunners(a_delta);
        static float ensureTimer = 0.0f;  // NPC 프로젝트 DB 에 이벤트 이름 등록 (외부 진입 이벤트가 오기 전에)
        Qte::Update(a_delta);
        GetUpHandoff::Update(a_delta);
        ActionLinks::Update(a_delta);
        Carry::Update(a_delta);
        PairRootRuntime::Sample(a_delta, true);
        NativePairEnd::Update(a_delta);
        NativePairAlignment::Update(a_delta);
        ViewGuard::Update(a_delta);
        ensureTimer -= a_delta;
        if (ensureTimer <= 0.0f) {
            ensureTimer = 2.0f;
            EventMaps::EnsureAllLoaded(AllEventNames());
        }
    }
    static inline REL::Relocation<decltype(thunk)> func;
};
}

void Load() {
    g_player.reset();
    g_playerBound = false;
    g_callbacks.Clear();
    g_loading = std::async(std::launch::async, [] {
        // An exception here would be rethrown in whichever thread reads the declarations first (a graph
        // loader, a message handler) and end the game. Nothing is declared then, and the reason is logged.
        try {
            return BuildDeclarations();
        } catch (const std::exception& e) {
            logger::error("[Actions] declarations could not be loaded: {}; no action is declared", e.what());
        } catch (...) {
            logger::error("[Actions] declarations could not be loaded (unknown error); no action is declared");
        }
        return Declared{};
    });
    logger::info("[Actions] declarations loading in the background");
}

ActionRunner* RunnerFor(RE::Actor* a_actor, int a_idx, bool a_create) {
    if (!a_actor || a_idx < 0 || a_idx >= Count()) return nullptr;
    if (a_actor->IsPlayerRef()) return PlayerRunner(a_idx, a_create);
    if (auto entry = FindNpc(a_actor->GetFormID()))
        return a_create ? entry->runners.Get(a_idx, MakeRunner(entry->handle, false)) : entry->runners.Find(a_idx);
    if (!a_create) return nullptr;
    auto entry = std::make_shared<NpcEntry>(a_actor->GetHandle(), Count());
    std::unique_lock lock(g_npcLock);
    const auto [it, inserted] = g_npc.try_emplace(a_actor->GetFormID(), std::move(entry));
    if (inserted)
        logger::debug("[Actions] NPC {:08X} '{}' 실행기 생성", a_actor->GetFormID(), a_actor->GetDisplayFullName());
    auto& found = *it->second;
    return found.runners.Get(a_idx, MakeRunner(found.handle, false));
}

std::shared_ptr<ActionRunner> RunnerRef(RE::Actor* a_actor, int a_idx) {
    if (!a_actor || a_idx < 0 || a_idx >= Count()) return nullptr;
    if (a_actor->IsPlayerRef()) {
        if (!g_playerBound.load(std::memory_order_acquire)) return nullptr;
        auto* runner = g_player->Find(a_idx);
        return runner ? std::shared_ptr<ActionRunner>(g_player, runner) : nullptr;
    }
    const auto entry = FindNpc(a_actor->GetFormID());
    auto* runner = entry ? entry->runners.Find(a_idx) : nullptr;
    return runner ? std::shared_ptr<ActionRunner>(entry, runner) : nullptr;
}

int NextRunner(RE::Actor* a_actor, int a_after, ActionRunner** a_runner) {
    *a_runner = nullptr;
    if (!a_actor) return -1;
    if (a_actor->IsPlayerRef()) {
        if (!g_playerBound.load(std::memory_order_acquire)) return -1;
        const int next = g_player->Next(a_after);
        *a_runner = next >= 0 ? g_player->Find(next) : nullptr;
        return next;
    }
    const auto entry = FindNpc(a_actor->GetFormID());
    const int next = entry ? entry->runners.Next(a_after) : -1;
    *a_runner = next >= 0 ? entry->runners.Find(next) : nullptr;
    return next;
}

void ClipActivated(RE::Actor* a_actor, int a_action, int a_variant, float a_duration, bool a_moves) {
    if (!a_actor || a_action < 0 || a_action >= Count()) return;
    if (MainThread::IsCurrent()) {
        ApplyClipActivated(a_actor, a_action, a_variant, a_duration, a_moves);
        return;
    }
    NoteOffMain(RunnerCallback::Kind::kActivated);
    PostCallback(
        {RunnerCallback::Kind::kActivated, a_actor->GetHandle(), a_action, a_variant, a_duration, {}, a_moves});
}

void ClipDeactivated(RE::Actor* a_actor, int a_action, int a_variant) {
    if (!a_actor || a_action < 0 || a_action >= Count()) return;
    if (MainThread::IsCurrent()) {
        ApplyClipDeactivated(a_actor, a_action, a_variant);
        return;
    }
    NoteOffMain(RunnerCallback::Kind::kDeactivated);
    PostCallback({RunnerCallback::Kind::kDeactivated, a_actor->GetHandle(), a_action, a_variant, 0.0f, {}});
}

void OnAnimEventFrom(RE::Actor* a_actor, const RE::BSFixedString& a_tag) {
    if (!a_actor || !a_tag.c_str()) return;
    PairFeatures::ObserveEvent(a_actor->GetHandle(), a_tag);
    EventRegistry::Observe(a_actor, a_tag);
    ActionLinks::ObserveEvent(a_actor->GetHandle(), a_tag);
    Qte::ObserveEvent(a_actor->GetHandle(), a_tag);
    GetUpHandoff::ObserveEvent(a_actor, a_tag);
    if (MainThread::IsCurrent())
        ApplyAnimEvent(a_actor, a_tag);
    else {
        NoteOffMain(RunnerCallback::Kind::kAnimEvent);
        PostCallback({RunnerCallback::Kind::kAnimEvent, a_actor->GetHandle(), -1, -1, 0.0f, a_tag});
    }
    if (QueueEventTrigger(a_actor, a_tag)) {
        const auto* state = a_actor->AsActorState();
        logger::debug("[TriggerSource] emitted actor={:08X} event='{}' sprinting={} (graph annotation / notify)",
            a_actor->GetFormID(), a_tag.c_str(), state && state->IsSprinting());
    }
}

std::vector<CapturedTrigger> CaptureEventTrigger(RE::Actor* actor, const RE::BSFixedString& event) {
    EnsureLoaded();
    std::vector<CapturedTrigger> captured;
    if (!actor || !event.c_str()) return captured;
    const bool player = actor->IsPlayerRef();
    // Almost no event is a trigger; the table is immutable, so look the event up first.
    const auto& table = EventTriggers();
    const auto found = event.empty() ? table.byTag.end() : table.byTag.find(event.c_str());
    if (found == table.byTag.end() || std::none_of(found->second.begin(), found->second.end(), [&](std::size_t i) {
            return player ? table.triggers[i].player : table.triggers[i].npc;
        }))
        return captured;
    const auto handle = actor->GetHandle();
    const auto* state = actor->AsActorState();
    const bool sprinting = state && state->IsSprinting();
    for (const std::size_t i : found->second) {
        const auto& t = table.triggers[i];
        if (!(player ? t.player : t.npc)) continue;
        RE::ActorHandle target;
        if (g_specs[t.action].trigger.eventCombatTarget) {
            target = actor->GetActorRuntimeData().currentCombatTarget;
            if (!target) {
                logger::warn("[EventTrigger] action='{}' has no combat target; request rejected", g_specs[t.action].id);
                continue;
            }
            logger::debug("[EventTrigger] captured target action='{}' actor={:08X} targetHandle={:08X}",
                g_specs[t.action].id, actor->GetFormID(), target.native_handle());
        }
        captured.push_back({handle, t.action, target, sprinting});
    }
    return captured;
}

bool CommitEventTrigger(const std::vector<CapturedTrigger>& captured) {
    if (captured.empty()) return false;
    bool matched = false;
    std::lock_guard lock(triggerMutex);
    for (const auto& c : captured) {
        if (std::any_of(triggerQueue.begin(), triggerQueue.end(),
                [&](const TriggerRequest& r) { return r.actor == c.actor && r.action == c.action; })) {
            matched = true;
            continue;
        }
        if (triggerQueue.size() < 64) {
            triggerQueue.push_back(c);
            matched = true;
        }
    }
    return matched;
}

bool QueueEventTrigger(RE::Actor* actor, const RE::BSFixedString& event) {
    return CommitEventTrigger(CaptureEventTrigger(actor, event));
}
void ResetEventTriggers() {
    std::lock_guard lock(triggerMutex);
    triggerQueue.clear();
    nativeQueue.clear();
}
void QueueNativeReplay(RE::ActorHandle actor, const RE::BSFixedString& event) {
    std::lock_guard lock(triggerMutex);
    if (nativeQueue.size() < 128) nativeQueue.push_back({actor, event});
}
// Runs on whatever thread notifies the graph; the runner is held by reference for the call.
bool CaptureNativeCancel(RE::Actor* actor, const RE::BSFixedString& event) {
    if (!actor || !actor->Is3DLoaded()) return false;
    const auto capture = [&](int, ActionRunner& r) { return r.CaptureNativeCancel(event); };
    if (actor->IsPlayerRef()) return g_playerBound.load(std::memory_order_acquire) && g_player->FindIf(capture);
    // Every graph event of every NPC comes here: one lookup, then only the runners it has used.
    const auto entry = FindNpc(actor->GetFormID());
    return entry && entry->runners.FindIf(capture);
}

int Count() {
    EnsureLoaded();
    return static_cast<int>(g_specs.size());
}
ActionSpec& Spec(int a_idx) {
    EnsureLoaded();
    return g_specs[a_idx];
}
ActionRunner& Runner(int a_idx) { return *PlayerRunner(a_idx, true); }
int IndexOf(std::string_view id) {
    EnsureLoaded();
    const auto it = g_idIndex.find(std::string(id));
    return it != g_idIndex.end() ? it->second : -1;
}
const std::vector<int>& ActionsForKey(std::uint32_t code) {
    EnsureLoaded();
    const auto it = g_keyActions.find(code);
    return it != g_keyActions.end() ? it->second : kNoActions;
}
const std::vector<int>& ActionsForGamepad(std::uint32_t button) {
    EnsureLoaded();
    const auto it = g_padActions.find(button);
    return it != g_padActions.end() ? it->second : kNoActions;
}
const std::vector<int>& FireArrowActions() {
    EnsureLoaded();
    return g_fireArrowActions;
}
const std::vector<std::pair<int, int>>& IncomingLinks(int action) {
    static const std::vector<std::pair<int, int>> none;
    EnsureLoaded();
    return action >= 0 && action < static_cast<int>(g_incomingLinks.size()) ? g_incomingLinks[action] : none;
}

bool FindClip(const char* a_clipName, int& a_actionIdx, int& a_variant) {
    EnsureLoaded();
    return g_clipIndex.Find(a_clipName, a_actionIdx, a_variant);
}

const std::vector<std::string>& AllClipFiles() {
    EnsureLoaded();
    return g_catalog.files;
}
const std::vector<std::string>& AllClipFiles(PairedRig::Kind rig) {
    EnsureLoaded();
    return g_catalog.ForRig(rig);
}
const std::vector<std::string>& FirstPersonClipFiles() {
    EnsureLoaded();
    return g_catalog.firstPersonFiles;
}
void CancelPlayerRun(const char* reason) {
    auto* player = RE::PlayerCharacter::GetSingleton();
    if (!player || !g_playerBound.load(std::memory_order_acquire)) return;
    // Pairs end through PairedSession::Cancel; their runners follow the pair.
    g_player->ForEach([&](int index, ActionRunner& runner) {
        if (!Spec(index).paired.enabled) runner.Cancel(reason);
    });
    ActionLinks::Cancel(player->GetHandle());
}
const std::vector<std::string>& AllEventNames() {
    EnsureLoaded();
    return g_catalog.uniqueEvents;
}
const std::vector<std::string>& RegistryEventNames() {
    EnsureLoaded();
    return g_catalog.registryEvents;
}

bool AnyActive() {
    return g_playerBound.load(std::memory_order_acquire) &&
           g_player->FindIf([](int, ActionRunner& r) { return r.GetState() != ActionRunner::State::kIdle; });
}

namespace {
// Read-only predicates, callable from any thread (damage and notify hooks).
template<class F>
bool AnyRunnerOf(RE::Actor* a_actor, F&& a_pred) {
    if (!a_actor) return false;
    const auto test = [&](int, ActionRunner& r) { return a_pred(r); };
    if (a_actor->IsPlayerRef()) return g_playerBound.load(std::memory_order_acquire) && g_player->FindIf(test);
    const auto entry = FindNpc(a_actor->GetFormID());
    return entry && entry->runners.FindIf(test);
}
}
bool HyperArmorFor(RE::Actor* a_actor) {
    return AnyRunnerOf(a_actor, [](const ActionRunner& r) { return r.IFrameActive() || r.HyperArmorActive(); });
}

void InstallHooks() {
    REL::Relocation<std::uintptr_t> vtbl{RE::VTABLE_PlayerCharacter[0]};
    PlayerUpdateHook::func = vtbl.write_vfunc(0xAD, PlayerUpdateHook::thunk);
    REL::Relocation<std::uintptr_t> pcSink{RE::VTABLE_PlayerCharacter[2]};
    REL::Relocation<std::uintptr_t> npcSink{RE::VTABLE_Character[2]};
    AnimEventHook<RE::PlayerCharacter>::func = pcSink.write_vfunc(0x1, AnimEventHook<RE::PlayerCharacter>::thunk);
    AnimEventHook<RE::Character>::func = npcSink.write_vfunc(0x1, AnimEventHook<RE::Character>::thunk);
    logger::info("[Actions] hooks installed (PlayerCharacter::Update, anim event sink PC/NPC)");
}

void BeforeGameLoad() {
    ExitBlendHooks::Reset();
    Qte::ResetOnLoad();
    GetUpHandoff::ResetOnLoad();
    NativePairAlignment::Reset(true);
    FireArrow::ResetOnLoad();
    Carry::BeforeGameLoad();
    PairRootRuntime::Reset();
    NativePairEnd::Reset();
    ResetEventTriggers();
    g_callbacks.Clear();
    for (auto& [id, entry] : NpcSnapshot()) entry->runners.ForEach([](int, ActionRunner& r) { r.ResetOnLoad(); });
    if (g_playerBound.load(std::memory_order_acquire)) g_player->ForEach([](int, ActionRunner& r) { r.ResetOnLoad(); });
}

void OnGameLoad() {
    ViewGuard::OnGameLoad();
    NativePairAlignment::Reset(false);
    FireArrow::ResetOnLoad();
    Carry::OnGameLoad();
    ExitBlendHooks::Reset();
    InputSink::Get()->Reset();
    PairRootRuntime::Reset();
    NativePairEnd::Reset();
    ResetEventTriggers();
    PairFeatures::ResetOnLoad();
    Qte::ResetOnLoad();
    GetUpHandoff::ResetOnLoad();
    ActionLinks::ResetOnLoad();
    AirControl::ResetOnLoad();
    PairedSession::ResetOnLoad();
    static int n = 0;
    BindPlayer();
    g_callbacks.Clear();
    const auto npcs = NpcSnapshot();
    for (auto& [id, e] : npcs) e->runners.ForEach([](int, ActionRunner& r) { r.ResetOnLoad(); });
    const std::size_t npcCount = npcs.size();
    {
        std::unique_lock lock(g_npcLock);
        g_npc.clear();
    }
    const bool bound = g_playerBound.load(std::memory_order_acquire);
    logger::info("[Actions] 게임 로드 #{} — 플레이어 실행기 {}개 리셋, NPC 실행기 {}명 폐기", ++n,
        bound ? g_player->Live() : 0, npcCount);  // V6 세이브/로드 반복 추적
    if (bound) g_player->ForEach([](int, ActionRunner& r) { r.ResetOnLoad(); });
    if (auto* p = RE::PlayerCharacter::GetSingleton()) EventMaps::Ensure(p, AllEventNames());
}

}  // namespace AnimationInjector::Actions
