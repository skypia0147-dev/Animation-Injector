#include "runtime/PartRunner.h"
#include "core/UpperLayer.h"
#include "runtime/ActionLinks.h"
#include "runtime/ActionRegistry.h"
#include "runtime/EventEffects.h"
#include "runtime/HeldKeys.h"
#include "runtime/InputSink.h"
#include "runtime/PairedSession.h"

#include <array>
#include <unordered_map>

namespace AnimationInjector::PartRunner {
namespace {
constexpr float kDefaultBlend = 0.3f;
constexpr float kDefaultExitBefore = 0.05f;
// An end or release transition was asked for (ActionLinks::ReachedEnd / ForceRelease said one waits) but the action
// still plays this long after: its window never opened. It blends out as it would have.
constexpr float kAskedGrace = 1.0f;
// Standing (UpperLayer::SetStand, user decision 11-21): the upper layer's clip takes the legs while the actor stands.
// At this speed (units/s) or above the legs are the locomotion's: under any walk (a test setup walks from about 80); the
// amount follows the speed as it rises from a stand, so the legs leave with the locomotion's start.
constexpr float kStandSpeed = 60.0f;
constexpr float kStandBlend = 0.25f;  // seconds for the whole way, at most (smooths the speed's steps)

enum class Phase : std::uint8_t { kIdle, kIn, kPlay, kOut };
struct Layer {
    int action = -1;
    int queued = -1;  // asked while the mix still crossed (or the layer left with the mix crossing): goes after
    Phase phase = Phase::kIdle;
    float w = 0.0f, written = -1.0f;
    // The slot the action plays in (0 A, 1 B): the end of the mix it stands at, or crosses to while `fading` (the
    // action it took over from) still plays in the other one.
    int slot = 0;
    int fading = -1;
    float mix = 0.0f, mixWritten = -1.0f, handBlend = kDefaultBlend;
    float elapsed = 0.0f;
    float blendIn = kDefaultBlend, blendOut = kDefaultBlend;
    bool endAsked = false, releaseAsked = false;
    float askedAt = 0.0f;
    std::vector<bool> timedFired;
};
struct Entry {
    RE::ActorHandle handle;
    bool player = false;
    bool wasSprinting = false;
    float stand = 0.0f, standWritten = -1.0f;  // the upper layer's standing amount (UpperLayer::SetStand)
    std::array<Layer, kBodyLayers> layers;
    std::unordered_map<int, float> cooldowns;  // action -> seconds left
};
std::unordered_map<RE::FormID, Entry> g_entries;  // main thread

// The player's sprint, stopped by a part action's start: the held sprint key's repeats are kept from the sprint
// handler until the key goes up or is pressed again (core: SprintHandler::ProcessButton).
std::atomic_bool g_sprintGate{false};

bool Active(const Layer& l) { return l.phase == Phase::kIn || l.phase == Phase::kPlay; }

using Names = std::array<RE::BSFixedString, kBodyLayers>;
Names MakeNames(const char* base) {
    const std::string b(base);
    return {RE::BSFixedString{(b + "0").c_str()}, RE::BSFixedString{(b + "1").c_str()},
        RE::BSFixedString{(b + "2").c_str()}};
}
const RE::BSFixedString& WeightName(std::size_t layer) {
    static const Names names = MakeNames(UpperLayer::kWeightVariable);
    return names[layer];
}
const RE::BSFixedString& MixName(std::size_t layer) {
    static const Names names = MakeNames(UpperLayer::kMixVariable);
    return names[layer];
}
const RE::BSFixedString& ClipName(std::size_t layer, int slot) {
    static const Names a = MakeNames(UpperLayer::kClipVariable);
    static const Names b = MakeNames(UpperLayer::kClipBVariable);
    return slot ? b[layer] : a[layer];
}

// Every graph of the actor that has the layer's variables, the inactive ones too: the actor's own Get/Set reach the
// active graph only (in first person the player's is the first-person graph, LocomotionDrive 11-20).
template<class F>
void ForGraphs(RE::Actor* actor, F&& f) {
    RE::BSAnimationGraphManagerPtr manager;
    if (!actor || !actor->GetAnimationGraphManager(manager) || !manager) return;
    for (auto& graph : manager->graphs)
        if (graph) f(*graph);
}
void WriteWeight(RE::Actor* actor, std::size_t layer, Layer& l) {
    if (l.written == l.w) return;
    const float value = std::min(l.w, 1.0f - UpperLayer::kBelowFloor);
    ForGraphs(actor, [&](RE::BShkbAnimationGraph& g) { g.SetGraphVariableFloat(WeightName(layer), value); });
    l.written = l.w;
}
void WriteStand(RE::Actor* actor, Entry& e) {
    if (e.standWritten == e.stand) return;
    ForGraphs(actor, [&](RE::BShkbAnimationGraph& g) { UpperLayer::SetStand(&g.characterInstance, e.stand); });
    e.standWritten = e.stand;
}
// The upper layer's standing amount: 1 at a stand, 0 from kStandSpeed and while sneaking (the clip's legs would
// stand the crouch up), no faster than kStandBlend for the whole way.
void UpdateStand(RE::Actor* actor, Entry& e, float delta) {
    static const RE::BSFixedString speedName{"Speed"};
    const auto& l = e.layers[0];
    float target = 0.0f;
    if (l.phase != Phase::kIdle || l.w > 0.0f) {
        float speed = 0.0f;
        actor->GetGraphVariableFloat(speedName, speed);
        target = actor->IsSneaking() ? 0.0f : std::clamp(1.0f - speed / kStandSpeed, 0.0f, 1.0f);
    }
    const float step = delta / kStandBlend;
    e.stand = std::clamp(target, e.stand - step, e.stand + step);
    WriteStand(actor, e);
}
void WriteMix(RE::Actor* actor, std::size_t layer, Layer& l) {
    if (l.mixWritten == l.mix) return;
    ForGraphs(actor, [&](RE::BShkbAnimationGraph& g) { g.SetGraphVariableFloat(MixName(layer), l.mix); });
    l.mixWritten = l.mix;
}
void WriteClip(RE::Actor* actor, std::size_t layer, int slot, int index) {
    ForGraphs(actor, [&](RE::BShkbAnimationGraph& g) { g.SetGraphVariableInt(ClipName(layer, slot), index); });
}

bool FirstPerson(RE::Actor* actor, bool player) {
    if (!player) return false;
    auto* camera = RE::PlayerCamera::GetSingleton();
    return camera && camera->IsInFirstPerson();
}

// Why `actor` cannot play a part action now (null: it can). `leaving`: a full-body action about to exit for a part
// action (a transition's source), not counted as playing.
const char* Blocked(RE::Actor* actor, int leaving = -1) {
    if (!actor || !actor->Is3DLoaded() || actor->IsDead()) return "dead or not loaded";
    auto* state = actor->AsActorState();
    if (!state) return "no actor state";
    if (state->GetKnockState() != RE::KNOCK_STATE_ENUM::kNormal || actor->IsInRagdollState()) return "knocked";
    if (state->GetSitSleepState() != RE::SIT_SLEEP_STATE::kNormal) return "sitting or sleeping";
    if (state->IsSwimming()) return "swimming";
    if (actor->IsOnMount()) return "mounted";
    if (actor->IsInKillMove()) return "killmove";
    PairedSession::Ticket ticket;
    if (PairedSession::TryGetTicket(actor->GetHandle(), ticket)) return "pair";
    if (state->GetAttackState() != RE::ATTACK_STATE_ENUM::kNone) return "attack";
    if (actor->IsBlocking()) return "block";
    switch (state->GetWeaponState()) {
    case RE::WEAPON_STATE::kWantToDraw:
    case RE::WEAPON_STATE::kDrawing:
    case RE::WEAPON_STATE::kWantToSheathe:
    case RE::WEAPON_STATE::kSheathing: return "weapon draw or sheathe";
    default: break;
    }
    for (const auto source : {RE::MagicSystem::CastingSource::kLeftHand, RE::MagicSystem::CastingSource::kRightHand,
             RE::MagicSystem::CastingSource::kOther})
        if (auto* caster = actor->GetMagicCaster(source)) {
            const auto s = caster->state.get();
            if (s == RE::MagicCaster::State::kUnk01 || s == RE::MagicCaster::State::kUnk02 ||
                s == RE::MagicCaster::State::kUnk04 || s == RE::MagicCaster::State::kCharging ||
                s == RE::MagicCaster::State::kCasting)
                return "casting";
        }
    if (auto* cc = actor->GetCharController();
        cc && (cc->context.currentState == RE::hkpCharacterStateType::kInAir ||
                  cc->context.currentState == RE::hkpCharacterStateType::kJumping))
        return "in the air";
    // One that is leaving (its exit sent, blending out) does not count: a transition from it to a part action, or
    // a part action started while it blends out, plays over its exit.
    if (Actions::BusyActive(actor, leaving)) return "a full-body action";
    return nullptr;
}

void RunTimed(RE::Actor* actor, const ActionSpec& spec, Layer& l) {
    const auto& timed = spec.pairFeatures.timed;
    if (timed.empty()) return;
    if (l.timedFired.size() != timed.size()) l.timedFired.assign(timed.size(), false);
    for (std::size_t i = 0; i < timed.size(); ++i)
        if (!l.timedFired[i] && l.elapsed >= timed[i].at) {
            l.timedFired[i] = true;
            const auto context = EventEffects::ContextFor(actor, "part action clip time");
            EventEffects::RunFor(timed[i].function, context, {actor->GetHandle()});
        }
}

// The action's own clock and blends, from its start.
void Arm(Layer& l, int action) {
    const auto& spec = Actions::Spec(action);
    l.action = action;
    l.queued = -1;
    l.elapsed = 0.0f;
    l.blendIn = spec.enterBlend >= 0.0f ? std::max(spec.enterBlend, 0.0f) : kDefaultBlend;
    l.blendOut = spec.exitBlend >= 0.0f ? std::max(spec.exitBlend, 0.0f) : kDefaultBlend;
    l.endAsked = l.releaseAsked = false;
    l.askedAt = 0.0f;
    l.timedFired.assign(spec.pairFeatures.timed.size(), false);
}
// The action stops being the layer's: its transitions close and its cooldown starts.
void Leave(Entry& e, const Layer& l) {
    if (l.action < 0) return;
    ActionLinks::End(e.handle, l.action);
    if (const float cooldown = Actions::Spec(l.action).condition.cooldown; cooldown > 0.0f)
        e.cooldowns[l.action] = cooldown;
}

// On an idle layer (w 0): the slot it stands at plays the clip from its start as w rises.
void Begin(RE::Actor* actor, Entry& e, std::size_t layer, int action) {
    auto& l = e.layers[layer];
    Arm(l, action);
    l.phase = Phase::kIn;
    l.fading = -1;
    l.mix = static_cast<float>(l.slot);
    WriteMix(actor, layer, l);
    // The clip index first: the clip child activates (from its start) on the first frame w is above 0.
    WriteClip(actor, layer, l.slot, UpperLayer::ClipIndex(action));
    ActionLinks::Begin(e.handle, action, false, e.player);
    const auto& spec = Actions::Spec(action);
    logger::info("[Part] {:08X} {} '{}' begins (enter {:.2f}s, exit {:.2f}s{})", actor->GetFormID(),
        BodyPartName(spec.body), spec.id, l.blendIn, l.blendOut, spec.loop.enabled ? ", loop" : "");
}

// On a layer whose weight is up and whose mix stands at one end: the action plays in the other slot, from its start
// (that slot weighed nothing), and the mix crosses to it over the action's enter blend.
void Handover(RE::Actor* actor, Entry& e, std::size_t layer, int action, const char* why) {
    auto& l = e.layers[layer];
    const int from = l.action;
    if (Active(l)) Leave(e, l);  // a layer that was leaving left already
    l.fading = from;
    l.slot = 1 - l.slot;
    Arm(l, action);
    l.handBlend = l.blendIn;
    l.phase = Phase::kIn;  // w goes on up (it was 1, or a leave turned back)
    WriteClip(actor, layer, l.slot, UpperLayer::ClipIndex(action));
    ActionLinks::Begin(e.handle, action, false, e.player);
    const auto& spec = Actions::Spec(action);
    logger::info("[Part] {:08X} {} '{}' takes over from '{}' ({}, crossing {:.2f}s{})", actor->GetFormID(),
        BodyPartName(spec.body), spec.id, from >= 0 ? Actions::Spec(from).id : "?", why, l.handBlend,
        spec.loop.enabled ? ", loop" : "");
}

void BeginOut(Entry& e, Layer& l, const char* why, RE::Actor* actor) {
    if (!Active(l)) return;
    Leave(e, l);
    l.phase = Phase::kOut;
    l.queued = -1;
    logger::info("[Part] {:08X} '{}' ends: {} (w {:.2f})", actor ? actor->GetFormID() : 0,
        l.action >= 0 ? Actions::Spec(l.action).id : "?", why, l.w);
}
void EndAtOnce(RE::Actor* actor, Entry& e, const char* why) {
    for (std::size_t i = 0; i < kBodyLayers; ++i) {
        auto& l = e.layers[i];
        if (l.phase == Phase::kIdle && l.w == 0.0f) continue;
        logger::info("[Part] {:08X} '{}' ends at once: {}", actor ? actor->GetFormID() : 0,
            l.action >= 0 ? Actions::Spec(l.action).id : "?", why);
        if (Active(l)) Leave(e, l);
        const int slot = l.slot;
        l = Layer{};
        l.slot = slot;
        l.mix = static_cast<float>(slot);
        if (actor) WriteWeight(actor, i, l);
    }
}

// The player's sprint stopped at a part action's start.
void StopSprint(RE::PlayerCharacter* player) {
    player->GetPlayerRuntimeData().playerFlags.isSprinting = false;
    g_sprintGate.store(true);
    logger::info("[Part] the player's sprint stopped for a part action; the held sprint key is ignored until "
                 "released or pressed again");
}

// SprintHandler::ProcessButton: while the gate is up, a held sprint key's repeats are dropped; its release or a new
// press goes through and takes the gate down.
struct SprintButton {
    static void thunk(RE::PlayerInputHandler* handler, RE::ButtonEvent* event, RE::PlayerControlsData* data) {
        if (g_sprintGate.load() && event) {
            if (event->IsDown() || event->IsUp())
                g_sprintGate.store(false);
            else
                return;
        }
        func(handler, event, data);
    }
    static inline REL::Relocation<decltype(thunk)> func;
};

// The checks of a start or a transition's destination. `leaving`: a full-body source exiting for it. `again`: the
// same action may play again (a transition to itself restarts it in the other slot).
bool Allowed(RE::Actor* actor, int action, int leaving, bool again) {
    if (!actor || action < 0 || action >= Actions::Count()) return false;
    const auto& spec = Actions::Spec(action);
    const int layer = LayerOf(spec.body);
    const auto deny = [&](const char* why) {
        logger::debug("[Part] '{}' refused: {}", spec.id, why);
        return false;
    };
    if (layer < 0) return false;
    if (!UpperLayer::LayerReady(layer) || UpperLayer::ClipIndex(action) < 0)
        return deny("its layer is not in the graph");
    const bool player = actor->IsPlayerRef();
    if (FirstPerson(actor, player)) return deny("first person");
    if (const char* why = Blocked(actor, leaving)) return deny(why);
    if (!player)
        if (auto* state = actor->AsActorState(); state && state->IsSprinting()) return deny("an NPC sprinting");
    if (const auto it = g_entries.find(actor->GetFormID()); it != g_entries.end()) {
        if (const auto c = it->second.cooldowns.find(action); c != it->second.cooldowns.end() && c->second > 0.0f)
            return deny("cooldown");
        const auto& l = it->second.layers[static_cast<std::size_t>(layer)];
        if (!again && l.action == action && Active(l)) return deny("already playing");
    }
    return true;
}

// The actor's entry, its sprint stopped for the player (a sprint the start stopped is not a sprint start a frame
// later: the engine clears its state next update).
Entry& Prepare(RE::Actor* actor) {
    auto& e = g_entries[actor->GetFormID()];
    e.handle = actor->GetHandle();
    e.player = actor->IsPlayerRef();
    auto* state = actor->AsActorState();
    e.wasSprinting = state && state->IsSprinting();
    if (e.player && e.wasSprinting) StopSprint(static_cast<RE::PlayerCharacter*>(actor));
    return e;
}

// The action on its layer: from w 0, over the one playing (the mix crosses), or after the crossing under way.
void Place(RE::Actor* actor, Entry& e, int action, const char* why) {
    const auto layer = static_cast<std::size_t>(LayerOf(Actions::Spec(action).body));
    auto& l = e.layers[layer];
    if (l.phase == Phase::kIdle && l.w <= 0.0f)
        Begin(actor, e, layer, action);
    else if (l.fading < 0)
        Handover(actor, e, layer, action, why);
    else {
        l.queued = action;
        logger::info("[Part] {:08X} '{}' waits for its layer's crossing", actor->GetFormID(), Actions::Spec(action).id);
    }
}

// The time its transitions read: a loop's counts each cycle (ActionRunner's, moving charge 10-30).
float ClipTime(const ActionSpec& spec, const Layer& l, float duration) {
    return spec.loop.enabled && duration > 0.05f ? std::fmod(l.elapsed, duration) : l.elapsed;
}
}  // namespace

bool CanStart(RE::Actor* actor, int action) { return Allowed(actor, action, -1, false); }

bool Start(RE::Actor* actor, int action, const char* origin) {
    if (!CanStart(actor, action)) return false;
    auto& e = Prepare(actor);
    logger::info("[Part] {:08X} '{}' asked ({})", actor->GetFormID(), Actions::Spec(action).id, origin ? origin : "?");
    Place(actor, e, action, "another start on its layer");
    return true;
}

bool CanLink(RE::Actor* actor, int source, int target) {
    if (!actor || source < 0 || source >= Actions::Count()) return false;
    const auto& from = Actions::Spec(source);
    if (!from.Part()) return Allowed(actor, target, source, false);
    // From a part action: its own layer plays on into the destination (itself again too).
    const bool sameLayer = LayerOf(from.body) == LayerOf(Actions::Spec(target).body);
    return Allowed(actor, target, -1, sameLayer);
}

bool Link(RE::Actor* actor, int source, int target) {
    if (!CanLink(actor, source, target)) return false;
    const auto& from = Actions::Spec(source);
    auto& e = Prepare(actor);
    logger::info("[Part] {:08X} '{}' -> '{}' (transition)", actor->GetFormID(), from.id, Actions::Spec(target).id);
    if (from.Part() && LayerOf(from.body) != LayerOf(Actions::Spec(target).body))
        EndAction(actor, source, "a transition to another layer");
    Place(actor, e, target, "transition");
    return true;
}

bool EndAction(RE::Actor* actor, int action, const char* why, float blend) {
    if (!actor || action < 0 || action >= Actions::Count() || !Actions::Spec(action).Part()) return false;
    const auto it = g_entries.find(actor->GetFormID());
    if (it == g_entries.end()) return false;
    auto& l = it->second.layers[static_cast<std::size_t>(LayerOf(Actions::Spec(action).body))];
    if (l.action != action || !Active(l)) return false;
    if (blend >= 0.0f) l.blendOut = blend;
    BeginOut(it->second, l, why, actor);
    return true;
}

void Update(float delta) {
    if (g_entries.empty() || !(delta > 0.0f)) return;
    for (auto it = g_entries.begin(); it != g_entries.end();) {
        auto& e = it->second;
        auto actorPtr = e.handle.get();
        auto* actor = actorPtr.get();
        for (auto c = e.cooldowns.begin(); c != e.cooldowns.end();)
            c = (c->second -= delta) <= 0.0f ? e.cooldowns.erase(c) : std::next(c);
        const bool any = std::any_of(
            e.layers.begin(), e.layers.end(), [](const Layer& l) { return l.phase != Phase::kIdle || l.w > 0.0f; });
        if (!any) {
            if (e.standWritten > 0.0f && actor) {
                e.stand = 0.0f;
                WriteStand(actor, e);
            }
            it = e.cooldowns.empty() ? g_entries.erase(it) : std::next(it);
            continue;
        }
        if (!actor || !actor->Is3DLoaded() || actor->IsDead()) {
            EndAtOnce(actor && actor->Is3DLoaded() ? actor : nullptr, e, "dead or unloaded");
            ++it;
            continue;
        }
        if (FirstPerson(actor, e.player)) {
            EndAtOnce(actor, e, "first person");
            ++it;
            continue;
        }
        // Interruptions: whatever put the actor in that state.
        const char* why = Blocked(actor);
        auto* state = actor->AsActorState();
        const bool sprinting = state && state->IsSprinting();
        if (!why && sprinting && !e.wasSprinting) why = "sprint start";
        e.wasSprinting = sprinting;
        for (std::size_t i = 0; i < kBodyLayers; ++i) {
            auto& l = e.layers[i];
            if (why && Active(l)) BeginOut(e, l, why, actor);
            if (l.phase == Phase::kIdle) continue;
            // The mix crossing to the slot that plays.
            if (l.fading >= 0) {
                const float target = static_cast<float>(l.slot);
                const float step = l.handBlend > 0.0f ? delta / l.handBlend : 1.0f;
                l.mix = target > l.mix ? std::min(target, l.mix + step) : std::max(target, l.mix - step);
                if (l.mix == target) {
                    l.fading = -1;
                    // A request that came while it crossed.
                    if (const int next = std::exchange(l.queued, -1); next >= 0 && Active(l)) {
                        if (Allowed(actor, next, -1, true)) Handover(actor, e, i, next, "after the crossing");
                    } else if (next >= 0)
                        l.queued = next;  // the layer is leaving: it begins once w is back at 0
                }
                WriteMix(actor, i, l);
            }
            const auto& spec = Actions::Spec(l.action >= 0 ? l.action : 0);
            switch (l.phase) {
            case Phase::kIn:
                l.w = l.blendIn > 0.0f ? std::min(1.0f, l.w + delta / l.blendIn) : 1.0f;
                if (l.w >= 1.0f) l.phase = Phase::kPlay;
                [[fallthrough]];
            case Phase::kPlay: {
                l.elapsed += delta;
                RunTimed(actor, spec, l);
                if (l.phase != Phase::kIn && l.phase != Phase::kPlay) break;  // a <Do at> ended it
                float duration = UpperLayer::ClipDuration(l.action);
                ActionLinks::ObserveTime(e.handle, l.action, ClipTime(spec, l, duration), duration);
                if (spec.loop.enabled) {
                    // maxHold counts from the press of the key that started the charge while it is down (a charge's
                    // start and hold keep one clock), else from this action's start; it counts as the key's release.
                    if (spec.loop.maxHold > 0.0f && !l.releaseAsked) {
                        const auto& held = HeldKeys::Get();
                        const float holding =
                            e.player && held.ChainHeld() ? held.ChainHeldTime(InputSink::Get()->Now()) : l.elapsed;
                        if (holding >= spec.loop.maxHold) {
                            l.releaseAsked = true;
                            l.askedAt = l.elapsed;
                            if (!ActionLinks::ForceRelease(e.handle, l.action)) BeginOut(e, l, "maxHold", actor);
                        }
                    } else if (l.releaseAsked && l.elapsed > l.askedAt + kAskedGrace)
                        BeginOut(e, l, "maxHold (its release transition did not go)", actor);
                    break;
                }
                // A clip played once blends out to end at its Exit time; its length is known once it activated. An
                // on="end" transition goes at that point instead.
                if (!(duration > 0.0f) && l.elapsed > spec.safetyDuration) duration = spec.safetyDuration;
                if (duration > 0.0f && !l.endAsked) {
                    const float exitAt =
                        duration - (spec.exitTriggerBeforeEnd >= 0.0f ? spec.exitTriggerBeforeEnd : kDefaultExitBefore);
                    if (l.elapsed >= exitAt - l.blendOut) {
                        l.endAsked = true;
                        l.askedAt = l.elapsed;
                        if (!ActionLinks::ReachedEnd(e.handle, l.action)) BeginOut(e, l, "its end", actor);
                    }
                } else if (l.endAsked && l.elapsed > l.askedAt + kAskedGrace)
                    BeginOut(e, l, "its end (its end transition did not go)", actor);
                break;
            }
            case Phase::kOut:
                l.elapsed += delta;
                l.w = l.blendOut > 0.0f ? std::max(0.0f, l.w - delta / l.blendOut) : 0.0f;
                if (l.w <= 0.0f) {
                    const int next = std::exchange(l.queued, -1);
                    l.phase = Phase::kIdle;
                    l.action = -1;
                    l.fading = -1;
                    l.mix = static_cast<float>(l.slot);
                    WriteWeight(actor, i, l);
                    WriteMix(actor, i, l);
                    if (next >= 0 && CanStart(actor, next)) Begin(actor, e, i, next);
                    continue;
                }
                break;
            case Phase::kIdle: break;
            }
            WriteWeight(actor, i, l);
        }
        UpdateStand(actor, e, delta);
        ++it;
    }
}

void End(RE::Actor* actor, int layer, const char* why) {
    if (!actor || layer < 0 || layer >= static_cast<int>(kBodyLayers)) return;
    if (const auto it = g_entries.find(actor->GetFormID()); it != g_entries.end())
        BeginOut(it->second, it->second.layers[static_cast<std::size_t>(layer)], why, actor);
}

void Reset() {
    for (auto& [id, e] : g_entries) {
        auto actor = e.handle.get();
        EndAtOnce(actor && actor->Is3DLoaded() ? actor.get() : nullptr, e, "load");
    }
    g_entries.clear();
    UpperLayer::ClearStand();
    g_sprintGate.store(false);
}

bool Running(RE::Actor* actor, int layer) {
    if (!actor) return false;
    const auto it = g_entries.find(actor->GetFormID());
    if (it == g_entries.end()) return false;
    for (std::size_t i = 0; i < kBodyLayers; ++i)
        if ((layer < 0 || layer == static_cast<int>(i)) && it->second.layers[i].phase != Phase::kIdle) return true;
    return false;
}

void Install() {
    REL::Relocation<std::uintptr_t> handler{RE::VTABLE_SprintHandler[0]};
    SprintButton::func = handler.write_vfunc(
        REL::VersionShift(0x04, RE::PlayerInputHandler::kAE1799AddedVFuncCount, SKSE::RUNTIME_SSE_1_7_99),
        SprintButton::thunk);
    logger::info("[Part] sprint button hook installed");
}

}  // namespace AnimationInjector::PartRunner
